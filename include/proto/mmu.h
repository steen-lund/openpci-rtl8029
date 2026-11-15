/* Automatically generated header (sfdc 1.13)! Do not edit! */

#ifndef PROTO_MMU_H
#define PROTO_MMU_H

#include <clib/mmu_protos.h>

#if defined(_CONST_BASES)
# ifndef __CONSTLIBBASEDECL__
# define __CONSTLIBBASEDECL__ const
# endif /* __CONSTLIBBASEDECL__ */
# ifndef __SEGMENTLIBBASEDECL__
# define __SEGMENTLIBBASEDECL__  __attribute__((__section__(".data")))
# endif /* __SEGMENTLIBBASEDECL__ */
#endif /* _CONST_BASES */
#ifdef __amigaos4__
# include <interfaces/mmu.h>
# ifndef __NOGLOBALIFACE__
   extern struct MMUIFace *IMMU;
# endif /* __NOGLOBALIFACE__*/
#endif /* !__amigaos4__ */
#ifndef __NOLIBBASE__
  extern struct Library *
# ifdef __CONSTLIBBASEDECL__
   __CONSTLIBBASEDECL__
# endif /* __CONSTLIBBASEDECL__ */
  MMUBase
# ifdef __SEGMENTLIBBASEDECL__
 __SEGMENTLIBBASEDECL__
# endif /* __SEGMENTLIBBASEDECL__ */
;
#endif /* !__NOLIBBASE__ */

#ifndef _NO_INLINE
# if defined(__GNUC__)
#  ifdef __AROS__
#   include <defines/mmu.h>
#  else
#   include <inline/mmu.h>
#  endif
# else
#  include <pragmas/mmu_pragmas.h>
# endif
#endif /* _NO_INLINE */

#endif /* !PROTO_MMU_H */
