/*
 *  gensio - A library for abstracting stream I/O
 *  Copyright (C) 2018  Corey Minyard <minyard@acm.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */

#ifndef GENSIO_ERR_H
#define GENSIO_ERR_H
#include <gensio/gensio_os_funcs.h>

#define GE_NOERR		0
#define GE_NOMEM		1
#define GE_NOTSUP		2
#define GE_INVAL		3
#define GE_NOTFOUND		4
#define GE_EXISTS		5
#define GE_OUTOFRANGE		6
#define GE_INCONSISTENT		7
#define GE_NODATA		8
#define GE_OSERR		9
#define GE_INUSE		10
#define GE_INPROGRESS		11
#define GE_NOTREADY		12
#define GE_TOOBIG		13
#define GE_TIMEDOUT		14
#define GE_RETRY		15
#define GE_errblank_xxx		16
#define GE_KEYNOTFOUND		17
#define GE_CERTREVOKED		18
#define GE_CERTEXPIRED		19
#define GE_KEYINVALID		20
#define GE_NOCERT		21
#define GE_CERTINVALID		22
#define GE_PROTOERR		23
#define GE_COMMERR		24
#define GE_IOERR		25
#define GE_REMCLOSE		26
#define GE_HOSTDOWN		27
#define GE_CONNREFUSE		28
#define GE_DATAMISSING		29
#define GE_CERTNOTFOUND		30
#define GE_AUTHREJECT		31
#define GE_ADDRINUSE		32
#define GE_INTERRUPTED		33

#define gensio_os_err_to_err(o, oserr)			\
    gensio_i_os_err_to_err(o, oserr, __FUNCTION__, __FILE__, __LINE__)

const char *gensio_err_to_str(int err);

int gensio_i_os_err_to_err(struct gensio_os_funcs *o,
			   int oserr, const char *caller,
			   const char *file, unsigned int lineno);

#endif /* GENSIO_ERR_H */
