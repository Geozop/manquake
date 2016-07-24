/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// net_udp.c

#include "quakedef.h"

#include <sys/types.h>

#ifdef WIN32
#include <winsock2.h>
#define MAXHOSTNAMELEN 256
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#endif

#include <errno.h>

#ifdef __sun__
#include <sys/filio.h>
#endif

#ifdef NeXT
#include <libc.h>
#endif

// Slot Zero 3.50-2  Fix compiler problems.
//extern int gethostname (char *, int);

extern int close (int);

extern cvar_t hostname;

static int net_acceptsocket = -1;		// socket for fielding new connections
static int net_controlsocket;
static int net_broadcastsocket = 0;
static struct qsockaddr broadcastaddr;

static unsigned long myAddr;

#include "net_udp.h"

//=============================================================================

int UDP_Init (void)
{
	char	buff[MAXHOSTNAMELEN];
	struct qsockaddr addr;
	char *colon;
	int i; // JPG - for checking command line

	if (COM_CheckParm ("-noudp"))
		return -1;

	// determine my name & address
	// JPG - check command line for -ip
	i = COM_CheckParm ("-ip");
	if (i)
	{
		if (i < com_argc-1)
		{
			myAddr = inet_addr(com_argv[i+1]);
			if (myAddr == INADDR_NONE)
				Sys_Error ("%s is not a valid IP address", com_argv[i+1]);
			strcpy(buff, com_argv[i+1]);
		}
		else
			Sys_Error ("NET_Init: you must specify an IP address after -ip");
	}
	else
	{
		// JPG 3.00 from CSR
		/*
		gethostname(buff, MAXHOSTNAMELEN);
		local = gethostbyname(buff);
		myAddr = *(int *)local->h_addr_list[0];
		*/
		myAddr = INADDR_ANY;
	}

	// JPG 3.02 - support for a trusted qsmack bot
	i = COM_CheckParm ("-qsmack");
	if (i)
	{
		if (i < com_argc-1)
		{
			qsmackAddr = inet_addr(com_argv[i+1]);
			if (qsmackAddr == INADDR_NONE)
				Sys_Error ("%s is not a valid IP address", com_argv[i+1]);
		}
		else
			Sys_Error ("NET_Init: you must specify an IP address after -qsmack");
	}
	else
		qsmackAddr = INADDR_ANY;

	// if the quake hostname isn't set, set it to the machine name
	if (strcmp(hostname.string, "UNNAMED") == 0)
	{
		buff[15] = 0;
		Cvar_Set ("hostname", buff);
	}

	if ((net_controlsocket = UDP_OpenSocket (0)) == -1)
		Sys_Error("UDP_Init: Unable to open control socket\n");

	((struct sockaddr_in *)&broadcastaddr)->sin_family = AF_INET;
	((struct sockaddr_in *)&broadcastaddr)->sin_addr.s_addr = INADDR_BROADCAST;
	((struct sockaddr_in *)&broadcastaddr)->sin_port = htons(net_hostport);

	UDP_GetSocketAddr (net_controlsocket, &addr);
	strcpy(my_tcpip_address,  UDP_AddrToString (&addr));
	colon = strrchr (my_tcpip_address, ':');
	if (colon)
		*colon = 0;

	Con_Printf("UDP Initialized\n");
	tcpipAvailable = true;

	return net_controlsocket;
}

//=============================================================================

void UDP_Shutdown (void)
{
	UDP_Listen (false);
	UDP_CloseSocket (net_controlsocket);
}

//=============================================================================

void UDP_Listen (qboolean state)
{
	// enable listening
	if (state)
	{
		if (net_acceptsocket != -1)
			return;
		if ((net_acceptsocket = UDP_OpenSocket (net_hostport)) == -1)
			Sys_Error ("UDP_Listen: Unable to open accept socket\n");
		return;
	}

	// disable listening
	if (net_acceptsocket == -1)
		return;
	UDP_CloseSocket (net_acceptsocket);
	net_acceptsocket = -1;
}

//=============================================================================

int UDP_OpenSocket (int port)
{
	int newsocket;
	struct sockaddr_in address;
	qboolean _true = true;

	if ((newsocket = socket (PF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
		return -1;

	if (ioctl (newsocket, FIONBIO, (char *)&_true) == -1)
		goto ErrorReturn;

	if (single_port_server && port)
	{
		int fReuse = 1;
		setsockopt(newsocket, SOL_SOCKET, SO_REUSEADDR, &fReuse, sizeof(fReuse));
	}

	memset(&address, 0, sizeof(struct sockaddr_in)); // JPG 1.05 - fix by JDC
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = myAddr; // JPG - changed INADDR_ANY to myAddr
	address.sin_port = htons(port);
	if( bind (newsocket, (void *)&address, sizeof(address)) == -1)
		goto ErrorReturn;

	return newsocket;

ErrorReturn:
	close (newsocket);
	return -1;
}

//=============================================================================

int UDP_CloseSocket (int socket)
{
	if (socket == net_broadcastsocket)
		net_broadcastsocket = 0;
	return close (socket);
}


//=============================================================================
/*
============
PartialIPAddress

this lets you type only as much of the net address as required, using
the local network components to fill in the rest
============
*/
static int PartialIPAddress (char *in, struct qsockaddr *hostaddr)
{
	char buff[256];
	char *b;
	int addr;
	int num;
	int mask;
	int run;
	int port;

	buff[0] = '.';
	b = buff;
	strcpy(buff+1, in);
	if (buff[1] == '.')
		b++;

	addr = 0;
	mask=-1;
	while (*b == '.')
	{
		b++;
		num = 0;
		run = 0;
		while (!( *b < '0' || *b > '9'))
		{
		  num = num*10 + *b++ - '0';
		  if (++run > 3)
		  	return -1;
		}
		if ((*b < '0' || *b > '9') && *b != '.' && *b != ':' && *b != 0)
			return -1;
		if (num < 0 || num > 255)
			return -1;
		mask<<=8;
		addr = (addr<<8) + num;
	}

	if (*b++ == ':')
		port = atoi(b);
	else
		port = net_hostport;

	hostaddr->sa_family = AF_INET;
	((struct sockaddr_in *)hostaddr)->sin_port = htons((short)port);
	((struct sockaddr_in *)hostaddr)->sin_addr.s_addr = (myAddr & htonl(mask)) | htonl(addr);

	return 0;
}
//=============================================================================

int UDP_Connect (int socket, struct qsockaddr *addr)
{
	if (single_port_server)
		return connect(socket, (struct sockaddr *)addr, sizeof(*addr));
	else
		return 0;
}

//=============================================================================

// JPG - replaced the old commented-out code with the code from net_wins.c to
//       fix the zero-size packet bug
int UDP_CheckNewConnections (void)
{
	char buf[4096];

	if (net_acceptsocket == -1)
		return -1;

	if (recvfrom (net_acceptsocket, buf, sizeof(buf), MSG_PEEK, NULL, NULL) >= 0)
	{
		return net_acceptsocket;
	}
	return -1;


	/*
	unsigned long	available;

	if (net_acceptsocket == -1)
		return -1;

	if (ioctl (net_acceptsocket, FIONREAD, &available) == -1)
		Sys_Error ("UDP: ioctlsocket (FIONREAD) failed\n");
	if (available)
		return net_acceptsocket;
	return -1;
	*/
}

//=============================================================================

int UDP_Read (int socket, byte *buf, int len, struct qsockaddr *addr)
{
	int addrlen = sizeof (struct qsockaddr);
	int ret;

	ret = recvfrom (socket, buf, len, 0, (struct sockaddr *)addr, &addrlen);
	if (ret == -1 && (errno == EWOULDBLOCK || errno == ECONNREFUSED))
		return 0;
	return ret;
}

//=============================================================================

int UDP_MakeSocketBroadcastCapable (int socket)
{
	int				i = 1;

	// make this socket broadcast capable
	if (setsockopt(socket, SOL_SOCKET, SO_BROADCAST, (char *)&i, sizeof(i)) < 0)
		return -1;
	net_broadcastsocket = socket;

	return 0;
}

//=============================================================================

int UDP_Write (int socket, byte *buf, int len, struct qsockaddr *addr)
{
	int ret;

	ret = sendto (socket, buf, len, 0, (struct sockaddr *)addr, sizeof(struct qsockaddr));
	if (ret == -1 && errno == EWOULDBLOCK)
		return 0;
	return ret;
}

//=============================================================================

char *UDP_AddrToString (struct qsockaddr *addr)
{
	static char buffer[22];
	int haddr;

	haddr = ntohl(((struct sockaddr_in *)addr)->sin_addr.s_addr);
	dpsnprintf(buffer, sizeof(buffer), "%d.%d.%d.%d:%d", (haddr >> 24) & 0xff, (haddr >> 16) & 0xff, (haddr >> 8) & 0xff, haddr & 0xff, ntohs(((struct sockaddr_in *)addr)->sin_port));
	return buffer;
}

//=============================================================================

int UDP_GetSocketAddr (int socket, struct qsockaddr *addr)
{
	int addrlen = sizeof(struct qsockaddr);
	unsigned int a;

	memset(addr, 0, sizeof(struct qsockaddr));
	getsockname(socket, (struct sockaddr *)addr, &addrlen);
	a = ((struct sockaddr_in *)addr)->sin_addr.s_addr;
	if (a == 0 || a == inet_addr("127.0.0.1"))
		((struct sockaddr_in *)addr)->sin_addr.s_addr = myAddr;

	return 0;
}

//=============================================================================

int UDP_GetAddrFromName(char *name, struct qsockaddr *addr)
{
	struct hostent *hostentry;

	if (name[0] >= '0' && name[0] <= '9')
		return PartialIPAddress (name, addr);

	hostentry = gethostbyname (name);
	if (!hostentry)
		return -1;

	addr->sa_family = AF_INET;
	((struct sockaddr_in *)addr)->sin_port = htons(net_hostport);
	((struct sockaddr_in *)addr)->sin_addr.s_addr = *(int *)hostentry->h_addr_list[0];

	return 0;
}

//=============================================================================

int UDP_AddrCompare (struct qsockaddr *addr1, struct qsockaddr *addr2)
{
	if (addr1->sa_family != addr2->sa_family)
		return -1;

	if (((struct sockaddr_in *)addr1)->sin_addr.s_addr != ((struct sockaddr_in *)addr2)->sin_addr.s_addr)
		return -1;

	if (((struct sockaddr_in *)addr1)->sin_port != ((struct sockaddr_in *)addr2)->sin_port)
		return 1;

	return 0;
}

//=============================================================================

int UDP_GetSocketPort (struct qsockaddr *addr)
{
	return ntohs(((struct sockaddr_in *)addr)->sin_port);
}

//=============================================================================
