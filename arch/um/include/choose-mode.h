/* 
 * Copyright (C) 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#ifndef __CHOOSE_MODE_H__
#define __CHOOSE_MODE_H__

#include "uml-config.h"

#if defined(UML_CONFIG_MODE_TT) && defined(UML_CONFIG_MODE_SKAS)
#define CHOOSE_MODE(tt, skas) (mode_tt ? (tt) : (skas))

#elif defined(UML_CONFIG_MODE_SKAS)
#define CHOOSE_MODE(tt, skas) (skas)

#elif defined(UML_CONFIG_MODE_TT)
#define CHOOSE_MODE(tt, skas) (tt)
#endif

#define CHOOSE_MODE_PROC(tt, skas, args...) \
	CHOOSE_MODE(tt(args), skas(args))

#endif

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
