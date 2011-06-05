/**
 * aligner_sw_sse.cpp
 *
 * Versions of key alignment functions that use vector instructions to
 * accelerate dynamic programming.  Based chiefly on the striped Smith-Waterman
 * paper and implementation by Michael Farrar.  See:
 *
 * Farrar M. Striped Smith-Waterman speeds database searches six times over
 * other SIMD implementations. Bioinformatics. 2007 Jan 15;23(2):156-61.
 * http://sites.google.com/site/farrarmichael/smith-waterman
 *
 * While the paper describes an implementation of Smith-Waterman, we extend it
 * do end-to-end read alignment as well as local alignment.  The change
 * required for this is minor: we simply let vmax be the maximum element in the
 * score domain rather than the minimum.
 *
 * The vectorized dynamic programming implementation lacks some features that
 * make it hard to adapt to solving the entire dynamic-programming alignment
 * problem.  For instance:
 *
 * - It doesn't respect gap barriers on either end of the read
 * - It just gives a maximum; not enough information to backtrace without
 *   redoing some alignment
 * - It's a little difficult to handle st_ and en_, especially st_.
 * - The query profile mechanism makes handling of ambiguous reference bases a
 *   little tricky (16 cols in query profile lookup table instead of 5)
 *
 * Given the drawbacks, it is tempting to use SSE dynamic programming as a
 * filter rather than as an aligner per se.  Here are a few ideas for how it
 * can be extended to handle more of the alignment problem:
 *
 * - Save calculated scores to a big array as we go.  We return to this array
 *   to find and backtrace from good solutions.
 */

#ifndef NO_SSE

#include "aligner_sw.h"

static const size_t NBYTES_PER_REG  = 16;
static const size_t NWORDS_PER_REG  = 16;
static const size_t NBITS_PER_WORD  = 8;
static const size_t NBYTES_PER_WORD = 1;

// In local mode, we start low (0) and go high (255).  Factoring in a query
// profile involves unsigned saturating addition.  All query profile elements
// should be expressed as a positive number; this is done by adding -min
// where min is the smallest (negative) score in the query profile.
	
typedef uint8_t TCScore;

/**
 * Build query profile look up tables for the read.  The query profile look
 * up table is organized as a 1D array indexed by [i][j] where i is the
 * reference character in the current DP column (0=A, 1=C, etc), and j is
 * the segment of the query we're currently working on.
 */
void SwAligner::buildQueryProfileLocalSseU8(bool fw) {
	const BTDnaString* rd = fw ? rdfw_ : rdrc_;
	const BTString* qu = fw ? qufw_ : qurc_;
	const size_t len = rd->length();
	const size_t seglen = (len + (NWORDS_PER_REG-1)) / NWORDS_PER_REG;
	// How many __m128i's are needed
	size_t n128s =
		64 +                    // slack bytes, for alignment?
		(seglen * ALPHA_SIZE)   // query profile data
		* 2;                    // & gap barrier data
	assert_gt(n128s, 0);
	SSEData& d = fw ? sseU8fw_ : sseU8rc_;
	d.buf_.resize(n128s * sizeof(__m128i));
	// Get a 16-byte aligned pointer toward the beginning of the buffer.
	size_t aligned = ((size_t)d.buf_.ptr() + 15) & ~(0x0f);
	// Set up pointers into the buffer for fw query
	d.qprof_       = reinterpret_cast<__m128i*>(aligned);
	d.maxPen_      = d.maxBonus_ = 0;
	d.lastIter_    = d.lastWord_ = 0;
	d.qprofStride_ = d.gbarStride_ = 2;
	d.bias_ = 0;
	// Calculate bias
	for(size_t refc = 0; refc < ALPHA_SIZE; refc++) {
		for(size_t i = 0; i < len; i++) {
			int readc = (*rd)[i];
			int readq = (*qu)[i];
			int sc = sc_->score(readc, (int)(1 << refc), readq - 33);
			if(sc < 0 && sc < d.bias_) {
				d.bias_ = sc;
			}
		}
	}
	assert_leq(d.bias_, 0);
	d.bias_ = -d.bias_;
	// For each reference character A, C, G, T, N ...
	for(size_t refc = 0; refc < ALPHA_SIZE; refc++) {
		// For each segment ...
		for(size_t i = 0; i < seglen; i++) {
			size_t j = i;
			uint8_t *qprofWords =
				reinterpret_cast<uint8_t*>(d.qprof_ + (refc * seglen * 2) + (i * 2));
			uint8_t *gbarWords =
				reinterpret_cast<uint8_t*>(d.qprof_ + (refc * seglen * 2) + (i * 2) + 1);
			// For each sub-word (byte) ...
			for(size_t k = 0; k < NWORDS_PER_REG; k++) {
				int sc = 0;
				*gbarWords = 0;
				if(j < len) {
					int readc = (*rd)[j];
					int readq = (*qu)[j];
					sc = sc_->score(readc, (int)(1 << refc), readq - 33);
					assert_range(0, 255, sc + d.bias_);
					size_t j_from_end = len - j - 1;
					if(j < (size_t)sc_->gapbar ||
					   j_from_end < (size_t)sc_->gapbar)
					{
						// Inside the gap barrier
						*gbarWords = std::numeric_limits<TCScore>::max();
					}
				}
				if(refc == 0 && j == len-1) {
					// Remember which 128-bit word and which smaller word has
					// the final row
					d.lastIter_ = i;
					d.lastWord_ = k;
				}
				if(sc < 0) {
					if((size_t)(-sc) > d.maxPen_) {
						d.maxPen_ = (size_t)(-sc);
					}
				} else {
					if((size_t)sc > d.maxBonus_) {
						d.maxBonus_ = (size_t)sc;
					}
				}
				*qprofWords = (uint8_t)(sc + d.bias_);
				gbarWords++;
				qprofWords++;
				j += seglen; // update offset into query
			}
		}
	}
}

/**
 * Solve the current alignment problem using SSE instructions that operate on 16
 * unsigned 8-bit values packed into a single 128-bit register.
 */
int SwAligner::alignNucleotidesLocalSseU8(int& flag) {
	assert_leq(rdf_, rd_->length());
	assert_leq(rdf_, qu_->length());
	assert_lt(rfi_, rff_);
	assert_lt(rdi_, rdf_);
	assert_eq(rd_->length(), qu_->length());
	assert_geq(sc_->gapbar, 1);
	assert(en_ == NULL || en_->size() == width_);
	assert(st_ == NULL || st_->size() == width_);
	assert(repOk());
#ifndef NDEBUG
	for(size_t i = rfi_; i < rff_; i++) {
		assert_range(0, 16, (int)rf_[i]);
	}
#endif

	SSEData& d = fw_ ? sseU8fw_ : sseU8rc_;
	assert(!d.buf_.empty());
	assert(d.qprof_ != NULL);

	assert_gt(d.maxBonus_, 0);
	size_t iter =
		(dpRows() + (NWORDS_PER_REG-1)) / NWORDS_PER_REG; // iter = segLen

	int dup;
	
	// Many thanks to Michael Farrar for releasing his striped Smith-Waterman
	// implementation:
	//
	//  http://sites.google.com/site/farrarmichael/smith-waterman
	//
	// Much of the implmentation below is adapted from Michael's code.

	// Set all elts to reference gap open penalty
	__m128i rfgapo   = _mm_setzero_si128();
	__m128i rfgape   = _mm_setzero_si128();
	__m128i rdgapo   = _mm_setzero_si128();
	__m128i rdgape   = _mm_setzero_si128();
	__m128i vlo      = _mm_setzero_si128();
	__m128i vfs      = _mm_setzero_si128();
	__m128i vvetohi  = _mm_setzero_si128();
	__m128i vhi      = _mm_setzero_si128();
	__m128i vmax     = _mm_setzero_si128();
	__m128i ve       = _mm_setzero_si128();
	__m128i vf       = _mm_setzero_si128();
	__m128i vh       = _mm_setzero_si128();
	__m128i vtmp     = _mm_setzero_si128();
	__m128i vzero    = _mm_setzero_si128();
	__m128i vbias    = _mm_setzero_si128();

	assert_gt(sc_->refGapOpen(), 0);
	assert_leq(sc_->refGapOpen(), std::numeric_limits<TCScore>::max());
	dup = (sc_->refGapOpen() << 8) | (sc_->refGapOpen() & 0x00ff);
	rfgapo = _mm_insert_epi16(rfgapo, dup, 0);
	rfgapo = _mm_shufflelo_epi16(rfgapo, 0);
	rfgapo = _mm_shuffle_epi32(rfgapo, 0);
	
	// Set all elts to reference gap extension penalty
	assert_gt(sc_->refGapExtend(), 0);
	assert_leq(sc_->refGapExtend(), std::numeric_limits<TCScore>::max());
	assert_leq(sc_->refGapExtend(), sc_->refGapOpen());
	dup = (sc_->refGapExtend() << 8) | (sc_->refGapExtend() & 0x00ff);
	rfgape = _mm_insert_epi16(rfgape, dup, 0);
	rfgape = _mm_shufflelo_epi16(rfgape, 0);
	rfgape = _mm_shuffle_epi32(rfgape, 0);

	// Set all elts to read gap open penalty
	assert_gt(sc_->readGapOpen(), 0);
	assert_leq(sc_->readGapOpen(), std::numeric_limits<TCScore>::max());
	dup = (sc_->readGapOpen() << 8) | (sc_->readGapOpen() & 0x00ff);
	rdgapo = _mm_insert_epi16(rdgapo, dup, 0);
	rdgapo = _mm_shufflelo_epi16(rdgapo, 0);
	rdgapo = _mm_shuffle_epi32(rdgapo, 0);
	
	// Set all elts to read gap extension penalty
	assert_gt(sc_->readGapExtend(), 0);
	assert_leq(sc_->readGapExtend(), std::numeric_limits<TCScore>::max());
	assert_leq(sc_->readGapExtend(), sc_->readGapOpen());
	dup = (sc_->readGapExtend() << 8) | (sc_->readGapExtend() & 0x00ff);
	rdgape = _mm_insert_epi16(rdgape, dup, 0);
	rdgape = _mm_shufflelo_epi16(rdgape, 0);
	rdgape = _mm_shuffle_epi32(rdgape, 0);
	
	// Set all elts to 0xffff
	vfs = _mm_cmpeq_epi16(vfs, vfs);
	vvetohi = _mm_slli_si128(vfs, NBYTES_PER_WORD);
	
	// Make a vector of bias offsets
	dup = (d.bias_ << 8) | (d.bias_ & 0x00ff);
	vbias = _mm_insert_epi16(vbias, dup, 0);
	vbias = _mm_shufflelo_epi16(vbias, 0);
	vbias = _mm_shuffle_epi32(vbias, 0);
	
	vhi = _mm_cmpeq_epi16(vhi, vhi); // all elts = 0xffff
	vlo = _mm_xor_si128(vlo, vlo);   // all elts = 0
	vmax = vlo;
	
	// Points to a long vector of __m128i where each element is a block of
	// contiguous cells in the E, F or H matrix.  If the index % 3 == 0, then
	// the block of cells is from the E matrix.  If index % 3 == 1, they're
	// from the F matrix.  If index % 3 == 2, then they're from the H matrix.
	// Blocks of cells are organized in the same interleaved manner as they are
	// calculated by the Farrar algorithm.
	const __m128i *pvScore; // points into the query profile

	d.mat_.init(dpRows(), rff_-rfi_, NWORDS_PER_REG);
	const size_t colstride = d.mat_.colstride();
	const size_t rowstride = d.mat_.rowstride();
	assert_eq(rowstride, colstride / iter);
	
	// Initialize the H and E vectors in the first matrix column
	__m128i *pvHTmp = d.mat_.tmpvec(0, 0);
	__m128i *pvETmp = d.mat_.evec(0, 0);
	
	for(size_t i = 0; i < iter; i++) {
		_mm_store_si128(pvETmp, vlo);
		_mm_store_si128(pvHTmp, vlo); // start low in local mode
		pvETmp += rowstride;
		pvHTmp += rowstride;
	}
	// These are swapped just before the innermost loop
	__m128i *pvHStore = d.mat_.hvec(0, 0);
	__m128i *pvHLoad  = d.mat_.tmpvec(0, 0);
	__m128i *pvELoad  = d.mat_.evec(0, 0);
	__m128i *pvEStore = d.mat_.evecUnsafe(0, 1);
	__m128i *pvFStore = d.mat_.fvec(0, 0);
	__m128i *pvFTmp   = NULL;
	
	assert_gt(sc_->gapbar, 0);
	
	// Fill in the table as usual but instead of using the same gap-penalty
	// vector for each iteration of the inner loop, load words out of a
	// pre-calculated gap vector parallel to the query profile.  The pre-
	// calculated gap vectors enforce the gap barrier constraint by making it
	// infinitely costly to introduce a gap in barrier rows.
	//
	// AND use a separate loop to fill in the first row of the table, enforcing
	// the st_ constraints in the process.  This is awkward because it
	// separates the processing of the first row from the others and might make
	// it difficult to use the first-row results in the next row, but it might
	// be the simplest and least disruptive way to deal with the st_ constraint.
	
	for(size_t i = rfi_; i < rff_; i++) {
		// Fetch the appropriate query profile.  Note that elements of rf_ must
		// be numbers, not masks.
		const int refc = (int)rf_[i];
		size_t off = (size_t)firsts5[refc] * iter * 2;
		pvScore = d.qprof_ + off; // even elts = query profile, odd = gap barrier
		
		// Set all cells to low value
		vf = _mm_xor_si128(vf, vf);

		// Load H vector from the final row of the previous column
		vh = _mm_load_si128(pvHLoad + colstride - rowstride);
		// Shift 2 bytes down so that topmost (least sig) cell gets 0
		vh = _mm_slli_si128(vh, NBYTES_PER_WORD);
		bool canStart = (st_ == NULL || i - rfi_ >= width_ || (*st_)[i - rfi_]);
		
		// We pull out one loop iteration to make it easier to veto values in the top row
		
		// Load cells from E, calculated previously
		ve = _mm_load_si128(pvELoad);
		pvELoad += rowstride;
		
		// Store cells in F, calculated previously
		_mm_store_si128(pvFStore, vf);
		pvFStore += rowstride;
		
		// Factor in query profile (matches and mismatches)
		vh = _mm_adds_epu8(vh, *pvScore);
		vh = _mm_subs_epu8(vh, vbias);
		if(!canStart) {
			vh = _mm_and_si128(vh, vvetohi);
		}
		pvScore++; // move on to the gap veto vector
		
		// Update H, factoring in E and F
		vh = _mm_max_epu8(vh, ve);
		vh = _mm_max_epu8(vh, vf);
		
		// Update highest score encountered this far
		vmax = _mm_max_epu8(vmax, vh);
		
		// Save the new vH values
		_mm_store_si128(pvHStore, vh);
		pvHStore += rowstride;
		
		// Update vE value
		vtmp = vh;
		vh = _mm_subs_epu8(vh, rdgapo);
		vh = _mm_subs_epu8(vh, *pvScore); // veto some read gap opens
		ve = _mm_subs_epu8(ve, rdgape);
		ve = _mm_max_epu8(ve, vh);
		
		// Load the next h value
		vh = _mm_load_si128(pvHLoad);
		pvHLoad += rowstride;
		
		// Save E values
		_mm_store_si128(pvEStore, ve);
		pvEStore += rowstride;
		
		// Update vf value
		vtmp = _mm_subs_epu8(vtmp, rfgapo);
		vtmp = _mm_subs_epu8(vtmp, *pvScore); // veto some ref gap opens
		vf = _mm_subs_epu8(vf, rfgape);
		vf = _mm_subs_epu8(vf, *pvScore); // veto some ref gap extensions
		vf = _mm_max_epu8(vf, vtmp);
		
		pvScore++; // move on to next query profile

		// For each character in the reference text:
		size_t j;
		for(j = 1; j < iter; j++) {
			// Load cells from E, calculated previously
			ve = _mm_load_si128(pvELoad);
			pvELoad += rowstride;
			
			// Store cells in F, calculated previously
			_mm_store_si128(pvFStore, vf);
			pvFStore += rowstride;
			
			// Factor in query profile (matches and mismatches)
			vh = _mm_adds_epu8(vh, *pvScore);
			vh = _mm_subs_epu8(vh, vbias);
			pvScore++; // move on to the gap veto vector
			
			// Update H, factoring in E and F
			vh = _mm_max_epu8(vh, ve);
			vh = _mm_max_epu8(vh, vf);
			
			// Update highest score encountered this far
			vmax = _mm_max_epu8(vmax, vh);
			
			// Save the new vH values
			_mm_store_si128(pvHStore, vh);
			pvHStore += rowstride;
			
			// Update vE value
			vtmp = vh;
			vh = _mm_subs_epu8(vh, rdgapo);
			vh = _mm_subs_epu8(vh, *pvScore); // veto some read gap opens
			ve = _mm_subs_epu8(ve, rdgape);
			ve = _mm_max_epu8(ve, vh);
			
			// Load the next h value
			vh = _mm_load_si128(pvHLoad);
			pvHLoad += rowstride;
			
			// Save E values
			_mm_store_si128(pvEStore, ve);
			pvEStore += rowstride;
			
			// Update vf value
			vtmp = _mm_subs_epu8(vtmp, rfgapo);
			vtmp = _mm_subs_epu8(vtmp, *pvScore); // veto some ref gap opens
			vf = _mm_subs_epu8(vf, rfgape);
			vf = _mm_subs_epu8(vf, *pvScore); // veto some ref gap extensions
			vf = _mm_max_epu8(vf, vtmp);
			
			pvScore++; // move on to next query profile
		}
		// pvHStore, pvELoad, pvEStore have all rolled over to the next column
		pvEStore -= colstride; // reset to start of column
		pvHStore -= colstride; // reset to start of column
		pvHLoad = pvHStore;    // new pvHLoad = pvHStore

		vh = _mm_load_si128(pvHStore);
		
		// Now I have the topmost vh and the bottommost vf

		// vf from last row gets shifted down by one to overlay the first row
		vf = _mm_slli_si128(vf, NBYTES_PER_WORD);
		
		// Charge a gap open to vh - this is one way we might update vf
		pvScore = d.qprof_ + off + 1; // reset veto vector
		vtmp = _mm_subs_epu8(vh, rfgapo);
		vtmp = _mm_subs_epu8(vtmp, *pvScore); // veto some ref gap opens
		vtmp = _mm_subs_epu8(vf, vtmp);
		vtmp = _mm_cmpeq_epi8(vtmp, vzero);
		int cmp = _mm_movemask_epi8(vtmp);
		
		// If any element of vtmp is greater than H - gap-open...
		j = 0;
		pvFTmp = pvFStore;
		pvFStore -= colstride;
		while(cmp != 0xffff) {
			// Grab the E vector that we calculated for the next column
			ve = _mm_load_si128(pvEStore);
			
			// Store this vf
			_mm_store_si128(pvFStore, vf);
			pvFStore += rowstride;
			
			// Update vh w/r/t new vf
			vh = _mm_max_epu8(vh, vf);
			
			// Save vH values
			_mm_store_si128(pvHStore, vh);
			pvHStore += rowstride;
			
			// Update highest score encountered this far
			vmax = _mm_max_epu8(vmax, vh);
			
			// Update E in case it can be improved using our new vh
			vh = _mm_subs_epu8(vh, rdgapo);
			vh = _mm_subs_epu8(vh, *pvScore); // veto some read gap opens
			ve = _mm_max_epu8(ve, vh);
			_mm_store_si128(pvEStore, ve);
			pvEStore += rowstride;

			// Update F with another gap extension
			vf = _mm_subs_epu8(vf, rfgape);
			vf = _mm_subs_epu8(vf, *pvScore); // veto some ref gap extensions
			pvScore += 2;

			assert_lt(j, iter);
			if(++j == iter) {
				pvHStore -= colstride;
				vh = _mm_load_si128(pvHStore);     // load next vh ASAP
				pvEStore -= colstride;
				pvFStore -= colstride;
				pvScore = d.qprof_ + off + 1;
				j = 0;
				vf = _mm_slli_si128(vf, NBYTES_PER_WORD);
			} else {
				vh = _mm_load_si128(pvHStore);     // load next vh ASAP
			}

			vtmp = _mm_subs_epu8(vh, rfgapo);
			vtmp = _mm_subs_epu8(vh, *pvScore); // veto some ref gap opens
			vtmp = _mm_subs_epu8(vf, vtmp);
			vtmp = _mm_cmpeq_epi8(vtmp, vzero);
			cmp  = _mm_movemask_epi8(vtmp);
		}

		// pvELoad and pvHLoad are already where they need to be
		
		// Adjust the load and store vectors here.  
		pvHStore = pvHLoad + colstride;
		pvEStore = pvELoad + colstride;
		pvFStore = pvFTmp;
	}

	// Find largest score in vmax
	vtmp = _mm_srli_si128(vmax, 8);
	vmax = _mm_max_epu8(vmax, vtmp);
	vtmp = _mm_srli_si128(vmax, 4);
	vmax = _mm_max_epu8(vmax, vtmp);
	vtmp = _mm_srli_si128(vmax, 2);
	vmax = _mm_max_epu8(vmax, vtmp);
	vtmp = _mm_srli_si128(vmax, 1);
	vmax = _mm_max_epu8(vmax, vtmp);

	int score = _mm_extract_epi16(vmax, 0);
	score = score & 0x00ff;

	// Could we have saturated?
	if(score + d.bias_ >= 255) {
		flag = -2; // yes
		return std::numeric_limits<int>::min();
	}
	
	// Did we find a solution?
	if(score == std::numeric_limits<TCScore>::min()) {
		flag = -1; // no
		return std::numeric_limits<int>::min();
	}
	
	// Return largest score
	return (int)score;
}

#endif