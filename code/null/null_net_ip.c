/*
===========================================================================
null_net_ip.c - Null network backend for the memtest / seL4 build.

Replaces net_ip.c entirely. All socket operations are no-ops; only
loopback (NA_LOOPBACK) addressing is supported, which is enough for
the dedicated-server game loop running bots against itself.

seL4 porting notes:
  - This file has zero OS dependencies beyond stdint types, making it
    a clean starting point for the seL4 port.
  - NET_Sleep() intentionally does not sleep; on seL4 replace it with
    seL4_Yield() or a timer IPC if you want to limit CPU spin.
===========================================================================
*/

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

/* fd_set is used only as a pass-through parameter; our implementations
   never touch the value.  Include the POSIX header on normal systems,
   or provide a dummy typedef when targeting seL4 (which has no select). */
#ifndef MEMTEST_SEL4
#  ifdef _WIN32
#    include <winsock2.h>
#  else
#    include <sys/select.h>
#    include <arpa/inet.h>   /* ntohs */
#  endif
#else
/* seL4 target: fd_set and ntohs are not available; provide stubs. */
typedef int fd_set;
static inline unsigned short ntohs_stub( unsigned short x ) { return x; }
#  define ntohs ntohs_stub
#endif

/* -----------------------------------------------------------------------
   Address comparison helpers
   These are pure computation — no syscalls.
   ----------------------------------------------------------------------- */

char *NET_ErrorString( void )
{
	return "";
}

/*
Sys_StringToAdr: parse a string into a netadr_t.
In the memtest build only "localhost" is meaningful.
*/
qboolean Sys_StringToAdr( const char *s, netadr_t *a, netadrtype_t family )
{
	memset( a, 0, sizeof( *a ) );
	if ( !strcmp( s, "localhost" ) )
	{
		a->type = NA_LOOPBACK;
		return qtrue;
	}
	Com_DPrintf( "Sys_StringToAdr: null backend ignoring address '%s'\n", s );
	return qfalse;
}

qboolean NET_CompareBaseAdrMask( netadr_t a, netadr_t b, int netmask )
{
	byte  cmpmask, *addra, *addrb;
	int   curbyte;

	if ( a.type != b.type )
		return qfalse;

	if ( a.type == NA_LOOPBACK )
		return qtrue;

	if ( a.type == NA_IP )
	{
		addra = (byte *) &a.ip;
		addrb = (byte *) &b.ip;
		if ( netmask < 0 || netmask > 32 )
			netmask = 32;
	}
	else if ( a.type == NA_IP6 )
	{
		addra = (byte *) &a.ip6;
		addrb = (byte *) &b.ip6;
		if ( netmask < 0 || netmask > 128 )
			netmask = 128;
	}
	else
	{
		return qfalse;
	}

	curbyte = netmask >> 3;
	if ( curbyte > 0 && memcmp( addra, addrb, curbyte ) )
		return qfalse;

	netmask &= 0x07;
	if ( netmask )
	{
		cmpmask = (byte)( 0xff00 >> netmask ) & 0xff;
		if ( ( addra[curbyte] & cmpmask ) != ( addrb[curbyte] & cmpmask ) )
			return qfalse;
	}
	return qtrue;
}

qboolean NET_CompareBaseAdr( netadr_t a, netadr_t b )
{
	if ( a.type != b.type )
		return qfalse;
	if ( a.type == NA_LOOPBACK )
		return qtrue;
	if ( a.type == NA_IP )
		return ( memcmp( a.ip, b.ip, 4 ) == 0 );
	if ( a.type == NA_IP6 )
		return ( memcmp( a.ip6, b.ip6, 16 ) == 0 );
	return qfalse;
}

qboolean NET_CompareAdr( netadr_t a, netadr_t b )
{
	if ( !NET_CompareBaseAdr( a, b ) )
		return qfalse;
	if ( a.type == NA_IP || a.type == NA_IP6 )
		return ( a.port == b.port );
	return qtrue;
}

qboolean NET_IsLocalAddress( netadr_t adr )
{
	return ( adr.type == NA_LOOPBACK );
}

qboolean Sys_IsLANAddress( netadr_t adr )
{
	/* No real network; nothing is a LAN address. */
	return qfalse;
}

void Sys_ShowIP( void )
{
}

/* -----------------------------------------------------------------------
   Address string formatting
   ----------------------------------------------------------------------- */

const char *NET_AdrToString( netadr_t a )
{
	static char s[64];
	if ( a.type == NA_LOOPBACK )
		Com_sprintf( s, sizeof( s ), "loopback" );
	else if ( a.type == NA_BOT )
		Com_sprintf( s, sizeof( s ), "bot" );
	else
		Com_sprintf( s, sizeof( s ), "<no-network>" );
	return s;
}

const char *NET_AdrToStringwPort( netadr_t a )
{
	static char s[64];
	if ( a.type == NA_LOOPBACK )
		Com_sprintf( s, sizeof( s ), "loopback" );
	else if ( a.type == NA_BOT )
		Com_sprintf( s, sizeof( s ), "bot" );
	else
		Com_sprintf( s, sizeof( s ), "<no-network>:%d", ntohs( a.port ) );
	return s;
}

/* -----------------------------------------------------------------------
   Multicast — no-ops in the null backend
   ----------------------------------------------------------------------- */

void NET_JoinMulticast6( void )
{
}

void NET_LeaveMulticast6( void )
{
}

/* -----------------------------------------------------------------------
   Packet I/O — all no-ops / returns-nothing-received
   ----------------------------------------------------------------------- */

/*
NET_GetPacket: called each frame to drain the OS receive buffer.
Returns qfalse always so the engine sees no incoming packets.
The loopback channel (sv_net_chan / cl_net_chan) bypasses this function
and works through the in-process loopback queue in net_chan.c.
*/
qboolean NET_GetPacket( netadr_t *net_from, msg_t *net_message, fd_set *fdr )
{
	return qfalse;
}

void Sys_SendPacket( int length, const void *data, netadr_t to )
{
	/* Drop the packet silently.  Loopback packets never reach here
	   because net_chan.c handles them through the loopback queue. */
}

/* -----------------------------------------------------------------------
   Lifecycle
   ----------------------------------------------------------------------- */

void NET_Config( qboolean enableNetworking )
{
}

void NET_Init( void )
{
	Com_Printf( "NET_Init: null network backend (memtest/seL4 build)\n" );
}

void NET_Shutdown( void )
{
}

void NET_Restart_f( void )
{
}

/*
NET_Sleep: in the real backend this blocks on select() waiting for a
packet or a timeout.  In the memtest build we skip sleeping so the
benchmark loop runs at full speed.

seL4 note: replace with seL4_Yield() or a seL4 timer IPC call if you
want to bound CPU usage during the benchmark.
*/
void NET_Sleep( int msec )
{
}

void NET_Event( fd_set *fdr )
{
}
