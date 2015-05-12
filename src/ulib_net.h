/*----------------------------------------------------------------------------
    ChucK Concurrent, On-the-fly Audio Programming Language
      Compiler and Virtual Machine

    Copyright (c) 2004 Ge Wang and Perry R. Cook.  All rights reserved.
      http://chuck.cs.princeton.edu/
      http://soundlab.cs.princeton.edu/

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
    U.S.A.
-----------------------------------------------------------------------------*/

//-----------------------------------------------------------------------------
// file: ulib_net.h
// desc: ...
//
// author: Ge Wang (gewang@cs.princeton.edu)
//         Perry R. Cook (prc@cs.princeton.edu)
// date: Spring 2004
//-----------------------------------------------------------------------------
#ifndef __ULIB_NET_H__
#define __ULIB_NET_H__

#include "chuck_dl.h"
#include "chuck_type.h"

// query func
DLL_QUERY net_query( Chuck_DL_Query * QUERY );
t_CKBOOL net_init();
t_CKBOOL net_shutdown();

// net interface

// netout
CK_DLL_CTOR ( netout_ctor );
CK_DLL_DTOR( netout_dtor );
CK_DLL_TICK( netout_tick );
CK_DLL_CTRL( netout_ctrl_addr );
CK_DLL_CGET( netout_cget_addr );
CK_DLL_CTRL( netout_ctrl_port );
CK_DLL_CGET( netout_cget_port );
CK_DLL_CTRL( netout_ctrl_size );
CK_DLL_CGET( netout_cget_size );
CK_DLL_CTRL( netout_ctrl_type );
CK_DLL_CGET( netout_cget_type );
CK_DLL_CTRL( netout_ctrl_seqnum );
CK_DLL_CGET( netout_cget_seqnum );
CK_DLL_MFUN( netout_start );

// netin
UGEN_CTOR netin_ctor( t_CKTIME now );
UGEN_DTOR netin_dtor( t_CKTIME now, void * data );
UGEN_TICK netin_tick( t_CKTIME now, void * data, SAMPLE in, SAMPLE * out );
UGEN_CTRL netin_ctrl_port( t_CKTIME now, void * data, void * value );
UGEN_CGET netin_cget_port( t_CKTIME now, void * data, void * out );
UGEN_CTRL netin_ctrl_seqnum( t_CKTIME now, void * data, void * value );
UGEN_CGET netin_cget_seqnum( t_CKTIME now, void * data, void * out );




#endif
