#include "altrep.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#include <mio/shared_mmap.hpp>
#pragma clang diagnostic pop

using namespace Rcpp;

// inspired by Luke Tierney and the R Core Team
// https://github.com/ALTREP-examples/Rpkg-mutable/blob/master/src/mutable.c
// and Romain François
// https://purrple.cat/blog/2018/10/21/lazy-abs-altrep-cplusplus/ and Dirk

struct readidx_string {

  static R_altrep_class_t class_t;

  // Make an altrep object of class `stdvec_double::class_t`
  static SEXP Make(
      std::shared_ptr<std::vector<size_t> >* offsets,
      mio::shared_mmap_source* mmap,
      R_xlen_t column,
      R_xlen_t num_columns,
      R_xlen_t skip) {

    // `out` and `xp` needs protection because R_new_altrep allocates
    SEXP out = PROTECT(Rf_allocVector(VECSXP, 5));

    SEXP xp = PROTECT(R_MakeExternalPtr(offsets, R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(xp, readidx_string::Finalize, TRUE);

    SEXP mmap_xp = PROTECT(R_MakeExternalPtr(mmap, R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(mmap_xp, readidx_string::Finalize_Mmap, TRUE);

    SET_VECTOR_ELT(out, 0, xp);
    SET_VECTOR_ELT(out, 1, mmap_xp);
    SET_VECTOR_ELT(out, 2, Rf_ScalarReal(column));
    SET_VECTOR_ELT(out, 3, Rf_ScalarReal(num_columns));
    SET_VECTOR_ELT(out, 4, Rf_ScalarReal(skip));

    // make a new altrep object of class `readidx_string::class_t`
    SEXP res = R_new_altrep(class_t, out, R_NilValue);

    UNPROTECT(3);

    return res;
  }

  // finalizer for the external pointer
  static void Finalize(SEXP xp) {
    auto vec_p = static_cast<std::shared_ptr<std::vector<size_t> >*>(
        R_ExternalPtrAddr(xp));
    delete vec_p;
  }

  static void Finalize_Mmap(SEXP xp) {
    auto mmap_p = static_cast<mio::shared_mmap_source*>(R_ExternalPtrAddr(xp));
    delete mmap_p;
  }

  static mio::shared_mmap_source* Mmap(SEXP x) {
    return static_cast<mio::shared_mmap_source*>(
        R_ExternalPtrAddr(VECTOR_ELT(R_altrep_data1(x), 1)));
  }

  static std::shared_ptr<std::vector<size_t> >* Ptr(SEXP x) {
    return static_cast<std::shared_ptr<std::vector<size_t> >*>(
        R_ExternalPtrAddr(VECTOR_ELT(R_altrep_data1(x), 0)));
  }

  // same, but as a reference, for convenience
  static std::shared_ptr<std::vector<size_t> >& Get(SEXP vec) {
    return *Ptr(vec);
  }

  static const R_xlen_t Column(SEXP vec) {
    return REAL(VECTOR_ELT(R_altrep_data1(vec), 2))[0];
  }

  static const R_xlen_t Num_Columns(SEXP vec) {
    return REAL(VECTOR_ELT(R_altrep_data1(vec), 3))[0];
  }

  static const R_xlen_t Skip(SEXP vec) {
    return REAL(VECTOR_ELT(R_altrep_data1(vec), 4))[0];
  }

  // ALTREP methods -------------------

  // The length of the object
  static R_xlen_t Length(SEXP vec) {
    return Get(vec)->size() / Num_Columns(vec);
  }

  // What gets printed when .Internal(inspect()) is used
  static Rboolean Inspect(
      SEXP x,
      int pre,
      int deep,
      int pvec,
      void (*inspect_subtree)(SEXP, int, int, int)) {
    Rprintf(
        "readidx_string (len=%d, ptr=%p materialized=%s)\n",
        Length(x),
        Ptr(x),
        R_altrep_data2(x) != R_NilValue ? "T" : "F");
    return TRUE;
  }

  // ALTSTRING methods -----------------

  // the element at the index `i`
  //
  // this does not do bounds checking because that's expensive, so
  // the caller must take care of that
  static SEXP string_Elt(SEXP vec, R_xlen_t i) {
    SEXP data2 = R_altrep_data2(vec);
    if (data2 != R_NilValue) {
      return STRING_ELT(data2, i);
    }
    auto sep_locs = Get(vec);
    auto column = Column(vec);
    auto num_columns = Num_Columns(vec);
    auto skip = Skip(vec);

    size_t idx = (i + skip) * num_columns + column;
    size_t cur_loc = (*sep_locs)[idx];
    size_t next_loc = (*sep_locs)[idx + 1];
    size_t len = next_loc - cur_loc;
    // Rcerr << cur_loc << ':' << next_loc << ':' << len << '\n';

    mio::shared_mmap_source* mmap = Mmap(vec);

    return Rf_mkCharLenCE(mmap->data() + cur_loc, len - 1, CE_UTF8);
  }

  // --- Altvec
  static SEXP Materialize(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if (data2 != R_NilValue) {
      return data2;
    }

    // allocate a standard character vector for data2
    R_xlen_t n = Length(vec);
    data2 = PROTECT(Rf_allocVector(STRSXP, n));

    for (R_xlen_t i = 0; i < n; ++i) {
      SET_STRING_ELT(data2, i, string_Elt(vec, i));
    }

    R_set_altrep_data2(vec, data2);
    UNPROTECT(1);
    return data2;
  }

  static void* Dataptr(SEXP vec, Rboolean writeable) {
    return STDVEC_DATAPTR(Materialize(vec));
  }

  static const void* Dataptr_or_null(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if (data2 == R_NilValue)
      return nullptr;

    return STDVEC_DATAPTR(data2);
  }

  // -------- initialize the altrep class with the methods above

  static void Init(DllInfo* dll) {
    class_t = R_make_altstring_class("readidx_string", "reagidx", dll);

    // altrep
    R_set_altrep_Length_method(class_t, Length);
    R_set_altrep_Inspect_method(class_t, Inspect);

    // altvec
    R_set_altvec_Dataptr_method(class_t, Dataptr);
    R_set_altvec_Dataptr_or_null_method(class_t, Dataptr_or_null);

    // altstring
    R_set_altstring_Elt_method(class_t, string_Elt);
  }
};

R_altrep_class_t readidx_string::class_t;

// Called the package is loaded (needs Rcpp 0.12.18.3)
// [[Rcpp::init]]
void init_readidx_string(DllInfo* dll) { readidx_string::Init(dll); }
