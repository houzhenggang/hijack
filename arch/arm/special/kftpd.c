// kftpd by Mark Lord
//
// This version can UPLOAD and DOWNLOAD files, directories..
//
// To Do:  fix various buffers to use PATH_MAX (4096) instead of 256 or 512 bytes

extern int hijack_khttpd_port;				// from arch/arm/special/hijack.c
extern int hijack_khttpd_verbose;			// from arch/arm/special/hijack.c
extern int hijack_kftpd_control_port;			// from arch/arm/special/hijack.c
extern int hijack_kftpd_data_port;			// from arch/arm/special/hijack.c
extern int hijack_kftpd_verbose;			// from arch/arm/special/hijack.c
extern struct semaphore hijack_khttpd_startup_sem;	// from arch/arm/special/hijack.c
extern struct semaphore hijack_kftpd_startup_sem;	// from arch/arm/special/hijack.c

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/smp_lock.h>
#include <linux/socket.h>
#include <linux/file.h>
#include <linux/net.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/proc_fs.h>
#include <linux/firewall.h>
#include <linux/wanrouter.h>
#include <linux/init.h>
#include <linux/poll.h>

#if defined(CONFIG_KMOD) && defined(CONFIG_NET)
#include <linux/kmod.h>
#endif

#include <asm/uaccess.h>

#include <linux/inet.h>
#include <net/ip.h>
#include <net/sock.h>
#include <net/rarp.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <net/scm.h>

#define INET_ADDRSTRLEN	16

#define CLIENT_CWD_SIZE		512
typedef struct server_parms_s {
	struct socket		*clientsock;
	struct socket		*servsock;
	struct socket		*datasock;
	struct sockaddr_in	clientaddr;
	int			verbose;
	int			use_http;
	int			data_port;
	unsigned int		umask;
	char			clientip[INET_ADDRSTRLEN];
	char			servername[8];
	int			have_portaddr;
	struct sockaddr_in	portaddr;
	unsigned char		cwd[CLIENT_CWD_SIZE];
} server_parms_t;

#define PRINTK	if (parms->verbose) printk

// This  function  converts  the  character string src
// into a network address structure in the af address family,
// then copies the network address structure to dst.
//
static int
inet_pton (int af, unsigned const char *src, void *dst)
{
	unsigned char	*d = dst;
	int		i;

	if (af != AF_INET)
		return -EAFNOSUPPORT;
	for (i = 3; i >= 0; --i) {
		unsigned int val = 0, digits = 0;
		while (*src >= '0' && *src <= '9') {
			val = (val * 10) + (*src++ - '0');
			++digits;
		}
		if (!digits || val > 255 || (i && *src++ != '.'))
			return 0;
		*d++ = val;
	}
	return 1;
}

// This  function  converts  the  network  address structure src
// in the af address family into a character string, which is copied
// to a character buffer dst, which is cnt bytes long.
//
static const char *
inet_ntop (int af, const void *src, char *dst, size_t cnt)
{
	unsigned const char *s = src;

	if (af != AF_INET || cnt < INET_ADDRSTRLEN)
		return NULL;
	sprintf(dst, "%u.%u.%u.%u", s[0], s[1], s[2], s[3]);
	return dst;
}

static int
extract_portaddr (struct sockaddr_in *addr, char *s)
{
	extern int	skip_atoi(char **s);	// from lib/vsprintf.c
	char		*first = s;
	int		dots = 0;

	memset(addr, 0, sizeof(struct sockaddr_in));
	addr->sin_family = AF_INET;
	while (*s && dots < 4) {
		if (*s == ',') {
			*s = '.';
			++dots;
		}
		++s;
	}
	*(s - 1) = '\0';
	if (inet_pton(AF_INET, first, &addr->sin_addr) > 0) {
		unsigned short port = (skip_atoi(&s) & 255) << 8;
		if (port && *s++) {
			port += skip_atoi(&s) & 255;
			if (port & 255) {
				addr->sin_port = htons(port);
				return 0;
			}
		}
	}
	return -1;
}

// Adapted from various examples in the kernel
static int
ksock_rw (struct socket *sock, char *buf, int buf_size, int minimum)
{
	struct msghdr	msg;
	struct iovec	iov;
	int		bytecount = 0, sending = 0;

	lock_kernel();
	if (minimum < 0) {
		minimum = buf_size;
		sending = 1;
	}
	do {
		int rc, len = buf_size - bytecount;

		iov.iov_base       = &buf[bytecount];
		iov.iov_len        = len;
		msg.msg_name       = NULL;
		msg.msg_namelen    = 0;
		msg.msg_iov        = &iov;
		msg.msg_iovlen     = 1;
		msg.msg_control    = NULL;
		msg.msg_controllen = 0;
		msg.msg_flags      = 0;

		if (sending)
			rc = sock_sendmsg(sock, &msg, len);
		else
			rc = sock_recvmsg(sock, &msg, len, 0);
		if (rc < 0 || (!sending && rc == 0))	// fixme? was: (rc <= 0)
			break;
		bytecount += rc;
	} while (bytecount < minimum);
	unlock_kernel();
	return bytecount;
}

typedef struct response_s {
	int		rcode;
	const char	*text;
} response_t;

static response_t response_table[] = {
	{150, "Opening BINARY connection"},
	{200, "Okay"},
	{202, "Okay"},
	{214, "Okay"},
	{215, "UNIX Type: L8"},
	{220, "Connected"},
	{221, "Happy Fishing"},
	{226, "Okay"},
	{230, "Okay"},
	{250, "Okay"},
	{257, "Okay"},
	{425, "Connection error"},
	{426, "Connection failed"},
	{451, "Internal error"},
	{500, "Bad command"},
	{501, "Bad syntax"},
	{502, "Unsupported xfer TYPE"},
	{550, "Failed"},
	{553, "Invalid action"},
	{0, NULL} // End-Of-Table Marker
	};

static int
send_response (server_parms_t *parms, int rcode)
{
	char		buf[64];
	int		len, rc;
	response_t	*r = response_table;

	while (r->rcode && r->rcode != rcode)
		++r;
	PRINTK("%s: %d %s.\n", parms->servername, rcode, r->text);
	len = sprintf(buf, "%d %s.\r\n", rcode, r->text);
	if ((rc = ksock_rw(parms->clientsock, buf, len, -1)) != len) {
		PRINTK("%s: ksock_rw(response) failed, rc=%d\n", parms->servername, rc);
		return -1;
	}
	return 0;
}

static int
send_dir_response (server_parms_t *parms, int rcode, char *dir, char *suffix)
{
	char	buf[300];
	int	rc, len;

	if (suffix)
		len = sprintf(buf, "%d \"%s\" %s\r\n", rcode, dir, suffix);
	else
		len = sprintf(buf, "%d \"%s\"\r\n", rcode, dir);
	if ((rc = ksock_rw(parms->clientsock, buf, len, -1)) != len) {
		PRINTK("%s: ksock_rw(dir_response) failed, rc=%d\n", parms->servername, rc);
		return 1;
	}
	return 0;
}

static int
send_help_response (server_parms_t *parms, char *type, char *commands)
{
	char	buf[128];
	int	rc, len;

	len = sprintf(buf, "214-The following %scommands are recognized (* =>'s unimplemented)\r\n", type);
	if (len != (rc = ksock_rw(parms->clientsock, buf, len, -1))
	 || len != (rc = ksock_rw(parms->clientsock, commands, len = strlen(commands), -1)))
	{
		PRINTK("%s: ksock_rw(help_response) failed, rc=%d\n", parms->servername, rc);
		return 1;
	}
	return 0;
}

static int
make_socket (server_parms_t *parms, struct socket **sockp, int port)
{
	int			rc, turn_on = 1;
	struct sockaddr_in	addr;
	struct socket		*sock;

	*sockp = NULL;
	if ((rc = sock_create(AF_INET, SOCK_STREAM, 0, &sock))) {
		PRINTK("%s: sock_create() failed, rc=%d\n", parms->servername, rc);
	} else if ((rc = sock_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&turn_on, sizeof(turn_on)))) {
		PRINTK("%s: setsockopt() failed, rc=%d\n", parms->servername, rc);
		sock_release(sock);
	} else {
		memset(&addr, 0, sizeof(struct sockaddr_in));
		addr.sin_family	     = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port        = htons(port);
		rc = sock->ops->bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
		if (rc) {
			PRINTK("%s: bind(port=%d) failed: %d\n", parms->servername, port, rc);
			sock_release(sock);
		} else {
			*sockp = sock;
		}
	}
	return rc;
}

static int
open_datasock (server_parms_t *parms)
{
	int		rc;
	unsigned int	response = 0;

	if (parms->use_http) {
		parms->datasock = parms->clientsock;
	} else if (!parms->have_portaddr) {
		response = 425;
	} else {
		parms->have_portaddr = 0;	// for next time
		if ((rc = make_socket(parms, &parms->datasock, hijack_kftpd_data_port))) {
			response = 425;
		} else {
			if (send_response(parms, 150)) {
				response = 451;	// this obviously will never get sent..
			} else {
				int flags = 0;
				rc = parms->datasock->ops->connect(parms->datasock,
					(struct sockaddr *)&parms->portaddr, sizeof(struct sockaddr_in), flags);
				if (rc)
					response = 426;
			}
			if (response)
				sock_release(parms->datasock);
		}
	}
	return response;
}

typedef struct tm_s
{
	int	tm_sec;		/* seconds	*/
	int	tm_min;		/* minutes	*/
	int	tm_hour;	/* hours	*/
	int	tm_mday;	/* day of month	*/
	int	tm_mon;		/* month	*/
	int	tm_year;	/* full year	*/
	int	tm_wday;	/* day of week	*/
	int	tm_yday;	/* days in year	*/
} tm_t;

#define	EPOCH_YR		(1970)		/* Unix EPOCH = Jan 1 1970 00:00:00 */
#define	SECS_PER_HOUR		(60L * 60L)
#define	SECS_PER_DAY		(24L * SECS_PER_HOUR)
#define	IS_LEAPYEAR(year)	(!((year) % 4) && (((year) % 100) || !((year) % 400)))
#define	DAYS_PER_YEAR(year)	(IS_LEAPYEAR(year) ? 366 : 365)

const int DAYS_PER_MONTH[2][12] = {
	{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },	// normal year
	{ 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 } };	// leap year

static char *MONTHS[12] =
	{ "Jan", "Feb", "Mar", "Apr", "May", "Jun",
	  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static tm_t *
convert_time (time_t time, tm_t *tm)
{
	unsigned long clock, day;
	const int *days_per_month;
	int month = 0, year = EPOCH_YR;

	clock   = (unsigned long)time % SECS_PER_DAY;
	day = (unsigned long)time / SECS_PER_DAY;
	tm->tm_sec   =  clock % 60;
	tm->tm_min   = (clock % SECS_PER_HOUR) / 60;
	tm->tm_hour  =  clock / SECS_PER_HOUR;
	tm->tm_wday  = (day + 4) % 7;	/* day 0 was a thursday */
	while ( day >= DAYS_PER_YEAR(year) ) {
		day -= DAYS_PER_YEAR(year);
		year++;
	}
	tm->tm_year  = year;
	tm->tm_yday  = day;
	days_per_month = DAYS_PER_MONTH[IS_LEAPYEAR(year)];
	month          = 0;
	while ( day >= days_per_month[month] ) {
		day -= days_per_month[month];
		month++;
	}
	tm->tm_mon   = month;
	tm->tm_mday  = day + 1;
	return tm;
}

static char *
format_time (tm_t *tm, int current_year, char *buf)
{
	int		t, y;
	const char	*s;

	s = MONTHS[tm->tm_mon];
	*buf++ = ' ';
	*buf++ = s[0];
	*buf++ = s[1];
	*buf++ = s[2];
	*buf++ = ' ';
	t = tm->tm_mday;
	*buf++ = (t / 10) ? '0' + (t / 10) : ' ';
	*buf++ = '0' + (t % 10);
	*buf++ = ' ';
	y = tm->tm_year;
	if (y == current_year) {
		t = tm->tm_hour;
		*buf++ = '0' + (t / 10);
		*buf++ = '0' + (t % 10);
		*buf++ = ':';
		t = tm->tm_min;
		*buf++ = '0' + (t / 10);
		*buf++ = '0' + (t % 10);
	} else {
		*buf++ = ' ';
		*buf++ = '0' +  (y / 1000);
		*buf++ = '0' + ((y /  100) % 10);
		*buf++ = '0' + ((y /   10) % 10);
		*buf++ = '0' +  (y %   10);
	}
	*buf = '\0';
	return buf;
}

#define MODE_XBIT(c,x,s)	((x) ? ((s) ? ((c)|0x20) : 'x') : ((s) ? (c) : '-'));

static char *
append_string (char *b, const char *s, int quoted)
{
	while (*s) {
		if (quoted && (*s == '\\' || *s == '"'))
			*b++ = '\\';
		*b++ = *s++;
	}
	return b;
}


// Pattern matching: returns 1 if n(ame) matches p(attern), 0 otherwise
static int
glob_match (char *n, char *p)
{
	while (*n && (*n == *p || *p == '?')) {
		++n;
		++p;
	}
	if (*p == '*') {
		while (*++p == '*');
		while (*n) {
			while (*n && (*n != *p && *p != '?'))
				++n;
			if (!*n)
				break;
			if (glob_match(n++, p))
				return 1;
		}
	}
	return (!(*n | *p));
}

typedef struct filldir_parms_s {
	int			current_year;	// current calendar year (YYYY), according to the Empeg
	int			full_listing;	// 0 == names only, 1 == "ls -al"
	unsigned long		blockcount;	// for "total xxxx" line at end of kftpd dir listing
	char			*pattern;	// for filename globbing (pattern matching for mget/mput/list)
	char			*name;		// points into path[]
	unsigned int		buf_size;	// size (bytes) of buf[]
	unsigned int		buf_len;	// number of bytes used buf[]
	char			*buf;		// allocated buffer for formatting partial dir listings
	unsigned int		nam_size;	// size (bytes) of nam[]
	unsigned int		nam_len;	// number of bytes used in nam[]
	char			*nam;		// allocated buffer for names from filldir()
	int			path_len;	// length of (non-zero terminated) base path in path[]
	char			path[512];	// full dir prefix, plus current name appended for dentry lookups
	char			lname[256];	// target path of a symbolic link
} filldir_parms_t;

// Callback routine for filp->f_op->readdir().
// This gets called repeatedly until we return non-zero (nam[] buffer full).
//
static int
filldir (void *parms, const char *name, int namelen, off_t offset, ino_t ino)
{
	filldir_parms_t *p = parms;
	char		*pname;

	if (name[0] == '.' && namelen <= 2) {
		if (namelen < 2 || (name[1] == '.' && p->path_len == 1 && p->path[0] == '/'))
			return 0;	// always skip "." ,  and skip ".." when showing rootdir
	}
	if ((p->nam_len + namelen + 1) > p->nam_size)
		return -EAGAIN; // stop reading directory entries
	pname = p->nam + p->nam_len;
	strncpy(pname, name, namelen);
	pname[namelen] = '\0';
	if (!p->pattern || glob_match(pname, p->pattern))
		p->nam_len += namelen + 1;	// keep this name
	return 0; // continue reading directory entries
}

const char dirlist_html_trailer[] = "</PRE><HR>\n</BODY></HTML>\n";
#define DIRLIST_TRAILER_MAX	(28)	// (sizeof(dirlist_html_trailer))

static int
send_dirlist_buf (server_parms_t *parms, filldir_parms_t *p, int send_trailer)
{
	int sent;

	if (p->full_listing && send_trailer) {
		if (parms->use_http)
			p->buf_len += sprintf(p->buf + p->buf_len, dirlist_html_trailer);
		else
			p->buf_len += sprintf(p->buf + p->buf_len, "total %lu\r\n", p->blockcount);
	}
	sent = ksock_rw(parms->datasock, p->buf, p->buf_len, -1);
	if (sent != p->buf_len) {
		PRINTK("%s: ksock_rw(%u) returned %d\n", parms->servername, p->buf_len, sent);
		return -EIO;
	}
	p->buf_len = 0;
	return 0;
}

static int	// callback routine for filp->f_op->readdir()
formatdir (server_parms_t *parms, filldir_parms_t *p, char *name, int namelen)
{
	struct dentry		*dentry;
	struct inode		*inode;
	unsigned long		mode;
	char			*b;
	tm_t			tm;
	char			ftype;
	int			rc, linklen = 0;

	if ((p->buf_size - p->buf_len) < (58 + namelen)) { 
		if ((rc = send_dirlist_buf(parms, p, 0)))	// empty the buffer
			return rc;
	}

	strcpy(p->name, name);		// fill in "tail" of p->path[]
	dentry = lnamei(p->path);
	if (IS_ERR(dentry) || !dentry->d_inode) {
		rc = PTR_ERR(dentry);
		PRINTK("%s: lnamei(%s) failed, rc=%d\n", parms->servername, p->path, rc);
		return -ENOENT;
	}
	inode = dentry->d_inode;
	mode = inode->i_mode;
	switch (mode & S_IFMT) {
		case S_IFLNK:	ftype = 'l'; break;
		case S_IFDIR:	ftype = 'd'; break;
		case S_IFCHR:	ftype = 'c'; break;
		case S_IFBLK:	ftype = 'b'; break;
		case S_IFIFO:	ftype = 'p'; break;
		case S_IFSOCK:	ftype = 's'; break;
		case S_IFREG:	ftype = '-'; break;
		default:	ftype = '-'; break;
	}
	if (!p->full_listing) {
		if (ftype == '-' || ftype == 'l') {
			b = append_string((p->buf + p->buf_len), name, 0);
			goto done;
		}
		dput(dentry); // free up the inode structure
		return 0;
	}

	b = p->buf + p->buf_len;
	*b++ = ftype;
	*b++ = (mode & S_IRUSR) ? 'r' : '-';
	*b++ = (mode & S_IWUSR) ? 'w' : '-';
	*b++ = MODE_XBIT('S', mode & S_IXUSR, mode & S_ISUID);
	*b++ = (mode & S_IRGRP) ? 'r' : '-';
	*b++ = (mode & S_IWGRP) ? 'w' : '-';
	*b++ = MODE_XBIT('S', mode & S_IXGRP, mode & S_ISGID);
	*b++ = (mode & S_IROTH) ? 'r' : '-';
	*b++ = (mode & S_IWOTH) ? 'w' : '-';
	*b++ = MODE_XBIT('T', mode & S_IXOTH, mode & S_ISVTX);

	b += sprintf(b, "%5u %-8u %-8u", inode->i_nlink, inode->i_uid, inode->i_gid);
	if (ftype == 'c' || ftype == 'b') {
		b += sprintf(b, " %3u, %3u", MAJOR(inode->i_rdev), MINOR(inode->i_rdev));
	} else {
		b += sprintf(b, " %8lu", inode->i_size);
		p->blockcount += inode->i_blocks;
	}

	b = format_time(convert_time(inode->i_mtime, &tm), p->current_year, b);
	*b++ = ' ';

	if (ftype == 'l') {
		extern int sys_readlink(const char *path, char *buf, int bufsiz);
		linklen = sys_readlink(p->path, p->lname, sizeof(p->lname));
		if (linklen <= 0) {
			PRINTK("%s: readlink(%s) failed, rc=%d\n", parms->servername, p->path, linklen);
			linklen = 0;
		}
	}
	p->lname[linklen] = '\0';

	p->buf_len = b - p->buf;
	if ((p->buf_size - p->buf_len) < ((parms->use_http ? (24+namelen) : 7) + namelen + (linklen ? (2 * linklen) : namelen))) {
		if ((rc = send_dirlist_buf(parms, p, 0))) {	// empty the buffer
			dput(dentry); // free up the inode structure
			return rc;
		}
		b = p->buf;
	}

	if (parms->use_http) {
		*b++ = '<';
		*b++ = 'A';
		*b++ = ' ';
		*b++ = 'H';
		*b++ = 'R';
		*b++ = 'E';
		*b++ = 'F';
		*b++ = '=';
		*b++ = '"';
		b = append_string(b, linklen ? p->lname : name, 1);
		if (ftype == 'd')
			*b++ = '/';
		*b++ = '"';
		*b++ = '>';
	}

	b = append_string(b, name, 0);
	if (parms->use_http) {
		if (ftype == 'd')
			*b++ = '/';
		*b++ = '<';
		*b++ = '/';
		*b++ = 'A';
		*b++ = '>';
	}

	if (linklen) {
		*b++ = ' ';
		*b++ = '-';
		*b++ = '>';
		*b++ = ' ';
		b = append_string(b, p->lname, 0);
	}
done:
	*b++ = '\r';
	*b++ = '\n';
	dput(dentry); // free up the inode structure

	p->buf_len = b - p->buf;
	if ((p->buf_size - p->buf_len) < DIRLIST_TRAILER_MAX) {
		if ((rc = send_dirlist_buf(parms, p, 0)))	// empty the buffer
			return rc;
	}
	return 0;
}

static const char dirlist_header[] =
	"HTTP/1.1 200 OK\n"
	"Connection: close\n"
	"Content-Type: text/html\n\n"
	"<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 3.2 Final//EN\">\n"
	"<HTML>\n"
	"<HEAD><TITLE>Index of %s</TITLE></HEAD>\n"
	"<BODY>\n"
	"<H2>Index of %s</H2>\n"
	"<PRE>\n"
	"<HR>\n";

static int
send_dirlist (server_parms_t *parms, char *path, int full_listing)
{
	int		rc, pathlen;
	struct file	*filp;
	unsigned int	response = 0;
	filldir_parms_t	p;

	p.pattern = NULL;
	pathlen = strlen(path);
	if (path[pathlen-1] == '/') {
		if (pathlen > 1)
			path[--pathlen] = '\0';
	} else {	// check for globbing in final path element:
		int globbing = 0;
		char *d = &path[pathlen];
		while (*--d != '/') {
			if (*d == '*' || *d == '?')
				++globbing;
		}
		if (globbing) {
			pathlen = d - path;
			if (pathlen == 0) {
				path = "/";
				pathlen = 1;
			}
			*d++ = '\0';
			p.pattern = d;
		}
	}
	lock_kernel();
	filp = filp_open(path,O_RDONLY,0);
	if (IS_ERR(filp) || !filp) {
		PRINTK("%s: filp_open(%s) failed\n", parms->servername, path);
		response = 550;
	} else {
		if (!filp->f_dentry || !filp->f_dentry->d_inode || !filp->f_op) {
			response = 550;
		} else if (!filp->f_op->readdir) {
			response = 553;
		} else if (!(p.buf = (char *)__get_free_page(GFP_KERNEL))) {
			response = 451;
		} else if (!(p.nam = (char *)__get_free_page(GFP_KERNEL))) {
			response = 451;
		} else if (!(response = open_datasock(parms))) {
			tm_t		tm;
			struct inode	*inode = filp->f_dentry->d_inode;

			p.current_year	= convert_time(CURRENT_TIME, &tm)->tm_year;
			p.blockcount	= 0;
			p.buf_size	= PAGE_SIZE;
			p.nam_size	= PAGE_SIZE;
			p.full_listing	= full_listing;
			strcpy(p.path, path);
			p.path_len	= pathlen;
			p.path[pathlen++] = '/';
			p.name		= p.path + pathlen;
			p.buf_len	= 0;

			if (parms->use_http)
				p.buf_len = sprintf(p.buf, dirlist_header, path, path);
			filp->f_pos = 0;
			do {
				p.nam_len = 0;
				down(&inode->i_sem);	// This can go inside the loop
				rc = filp->f_op->readdir(filp, &p, filldir); // anything "< 0" is an error
				up(&inode->i_sem);
				if (rc < 0) {
					PRINTK("%s: readdir() returned %d\n", parms->servername, rc);
				} else {
					unsigned int pos = 0;
					rc = 0;
					while (pos < p.nam_len) {
						char *name = p.nam + pos;
						int namelen = strlen(name);
						pos += namelen + 1;
						rc = formatdir(parms, &p, name, namelen);
						if (rc < 0) {
							PRINTK("%s: formatdir('%s') returned %d\n", parms->servername, p.name, rc);
							break;
						}
					}
				}
			} while (!rc && p.nam_len);
			if (rc || (rc = send_dirlist_buf(parms, &p, 1)))
				response = 426;
			if (!parms->use_http)
				sock_release(parms->datasock);
			if (p.nam)
				free_page((unsigned long)p.nam);
			if (p.buf)
				free_page((unsigned long)p.buf);
		}
		filp_close(filp,NULL);
	}
	unlock_kernel();
	return response;
}

static const char http_redirect[] =
	"HTTP/1.1 302 Found\n"
	"Location: %s/\n"
	"Connection: close\n"
	"Content-Type: text/html\n\n"
	"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n"
	"<HTML><HEAD>\n"
	"<TITLE>302 Found</TITLE>\n"
	"</HEAD><BODY>\n"
	"<H1>Found</H1>\n"
	"The document has moved <A HREF=\"%s/\">here</A>.<P>\n"
	"</BODY></HTML>\n";

static void
khttpd_dir_redirect (server_parms_t *parms, const char *path, char *buf)
{
	unsigned int	len, rc;

	len = sprintf(buf, http_redirect, path, path);
	rc = ksock_rw(parms->clientsock, buf, len, -1);
	if (rc != len)
		PRINTK("%s: bad_request(): ksock_rw(%d) returned %d\n", parms->servername, len, rc);
}

static int
khttp_send_file_header (server_parms_t *parms, char *path, unsigned long i_size, char *buf)
{
	char *hdr  = "HTTP/1.1 200 OK\nConnection: close\nAccept-Ranges: bytes\nContent-Type: %s\n";
	char *type = "text/plain"; // or maybe: "application/octet-stream"
	int len;
	const char *s;

	// Very crude "tune" recognition scheme:
	if (i_size > 1000) {
		s = path + strlen(path);
		if (*--s == '0') {
			while (s > path && *--s != '/');
			while (s > path && *--s != '/');
			if (!strncmp(s, "/fids", 5))
				type = "audio/mpeg";	// or "application/octet-stream" ??
		}
	}
	len = sprintf(buf, hdr, type);
	if (i_size)
		len += sprintf(buf+len, "Content-Length: %lu\n", i_size);
	buf[len++] = '\n';
	if (len != ksock_rw(parms->datasock, buf, len, -1))
		return 426;
	return 0;
}

static int
send_file (server_parms_t *parms, char *path)
{
	unsigned int	size;
	struct file	*filp;
	unsigned int	response = 0;
	unsigned char	*buf;

	lock_kernel();
	if (!(buf = (unsigned char *)__get_free_page(GFP_KERNEL))) {
		response = 451;
	} else {
		filp = filp_open(path,O_RDONLY,0);
		if (IS_ERR(filp) || !filp) {
			PRINTK("%s: filp_open(%s) failed\n", parms->servername, path);
			response = 550;
		} else {
			if (!filp->f_dentry || !filp->f_dentry->d_inode || !filp->f_op) {
				response = 550;
			} else if (filp->f_op->readdir) {
				if (parms->use_http)
					khttpd_dir_redirect(parms, path, buf);
				else
					response = 553;
			} else if (!filp->f_op->read) {
				response = 550;
			} else if (!(response = open_datasock(parms))) {
				if (!parms->use_http || !(response = khttp_send_file_header(parms, path, filp->f_dentry->d_inode->i_size, buf))) {
					filp->f_pos = 0;
					do {
						size = filp->f_op->read(filp, buf, PAGE_SIZE, &(filp->f_pos));
						if (size < 0) {
							PRINTK("%s: filp->f_op->read() failed; rc=%d\n", parms->servername, size);
							response = 451;
							break;
						} else if (size && size != ksock_rw(parms->datasock, buf, size, -1)) {
							response = 426;
							break;
						}
					} while (size > 0);
				}
				if (!parms->use_http)
					sock_release(parms->datasock);
			}
			filp_close(filp,NULL);
		}
		free_page((unsigned long)buf);
	}
	unlock_kernel();
	return response;
}

// cloned from fs/read_write.c
//
static inline loff_t llseek(struct file *file, loff_t offset, int origin)
{
	extern loff_t default_llseek(struct file *file, loff_t offset, int origin); // fs/read_write.c
	loff_t (*fn)(struct file *, loff_t, int);

	fn = default_llseek;
	if (file->f_op && file->f_op->llseek)
		fn = file->f_op->llseek;
	return fn(file, offset, origin);
}

static int
do_rmdir (server_parms_t *parms, const char *path)
{
	extern int	sys_rmdir(const char *path); // fs/namei.c
	int		rc, response = 250;;

	if ((rc = sys_rmdir(path))) {
		PRINTK("%s: rmdir('%s') failed, rc=%d\n", parms->servername, path, rc);
		response = 550;
	}
	return response;
}

static int
do_mkdir (server_parms_t *parms, const char *path)
{
	extern int	sys_mkdir(const char *path, int mode); // fs/namei.c
	int		rc, response = 257;

	if ((rc = sys_mkdir(path, 0777 & ~parms->umask))) {
		PRINTK("%s: mkdir('%s') failed, rc=%d\n", parms->servername, path, rc);
		response = 550;
	}
	return response;
}

static int
do_chmod (server_parms_t *parms, unsigned int mode, const char *path)
{
	extern int	sys_chmod(const char *path, mode_t mode); // fs/open.c
	int		rc, response = 200;

	if ((rc = sys_chmod(path, mode))) {
		PRINTK("%s: chmod('%s',%d) failed, rc=%d\n", parms->servername, path, mode, rc);
		response = 550;
	}
	return response;
}

static int
do_delete (server_parms_t *parms, const char *path)
{
	extern int	sys_unlink(const char *path);
	int		rc, response = 250;

	if ((rc = sys_unlink(path))) {
		PRINTK("%s: unlink('%s') failed, rc=%d\n", parms->servername, path, rc);
		response = 550;
	}
	return response;
}

static int
receive_file (server_parms_t *parms, const char *path)
{
	int		size, rc;
	struct file	*filp;
	unsigned int	response = 0;
	unsigned char	*buf;

	lock_kernel();
	if (!(buf = (unsigned char *)__get_free_page(GFP_KERNEL))) {
		response = 451;
	} else {
		filp = filp_open(path,O_CREAT|O_TRUNC|O_RDWR, 0666 & ~parms->umask);
		if (IS_ERR(filp) || !filp) {
			PRINTK("%s: filp_open(%s) failed\n", parms->servername, path);
			response = 550;
		} else {
			if (filp->f_op->readdir) {
				response = 553;
			} else if (!filp->f_dentry || !filp->f_dentry->d_inode || !filp->f_op || !filp->f_op->write) {
				response = 550;
			} else if (!(response = open_datasock(parms))) {
				if (llseek(filp, 0, 0) < 0) {
					response = 550;
				} else {
					filp->f_pos = 0;
					do {
						size = ksock_rw(parms->datasock, buf, PAGE_SIZE, 1);
						if (size < 0) {
							response = 426;
							break;
						} else if (size != (rc = filp->f_op->write(filp, buf, size, &(filp->f_pos)))) {
							PRINTK("%s: filp->f_op->write(%d) failed; rc=%d\n", parms->servername, size, rc);
							response = 451;
							break;
						}
					} while (size > 0);
				}
				sock_release(parms->datasock);
			}
			filp_close(filp,NULL);
		}
		free_page((unsigned long)buf);
	}
	unlock_kernel();
	return response;
}

static void
append_path (char *path, char *new)
{
	char	buf[CLIENT_CWD_SIZE], *b = buf, *p = path;

	if (*new == '/') {
		strcpy(buf, new);
	} else {
		strcpy(buf, path);
		strcat(buf, "/");
		strcat(buf, new);
	}
	// Now fix-up the path, resolving '..' and removing consecutive '/'
	*p++ = '/';
	while (*b) {
		while (*b == '/')
			++b;
		if (b[0] == '.' && b[1] == '.' && (b[2] == '/' || b[2] == '\0')) {
			b += 2;
			while (*--p != '/');
			if (p == path)
				++p;
		} else if (*b) { // copy simple path element
			if (*(p-1) != '/')
				*p++ = '/';
			while (*b && *b != '/')
				*p++ = *b++;
		}
	}
	*p = '\0';
}

/////////////////////////////////////////////////////////////////////////////////
//
// In order to make FTP workable without needless error messages, the
// following minimum implementation is required for all servers:
//
//	TYPE - ASCII Non-print
//	MODE - Stream
//	STRUCTURE - File, Record
//	COMMANDS - USER, QUIT, PORT, RETR, STOR, NOOP.
//	    and  - TYPE, MODE, STRU (for at least the default values)
//
// The default values for transfer parameters are:
//
//         TYPE - ASCII Non-print
//         MODE - Stream
//         STRU - File
//
// All hosts must accept the above as the standard defaults.
//
// In addition, we also need:  NLST, PWD, CWD, CDUP, MKD, RMD, DELE, and maybe SYST
// The ABOR command Would Be Nice.  Plus, the UMASK, CHMOD, and EXEC extensions (below).
//
// Non-standard UNIX extensions from wu-ftpd: "SITE EXEC", "SITE CHMOD", "SITE HELP", maybe "MINFO".
//
//	Request	Description
//	UMASK	change umask. E.g. SITE UMASK 002
//	CHMOD	change mode of a file. E.g. SITE CHMOD 755 filename
//	HELP	give help information. E.g. SITE HELP
//	NEWER	list files newer than a particular date
//	MINFO	like SITE NEWER, but gives extra information
//	GPASS	give special group access password. E.g. SITE GPASS bar
//	EXEC	execute a program.	E.g. SITE EXEC program params
//
/////////////////////////////////////////////////////////////////////////////////


static void
make_keyword_uppercase (char *keyword)
{
	char c = *keyword;

	while (c && c != ' ') {
		if (c >= 'a' && c <= 'z')
			*keyword -= ('a' - 'A');
		c = *++keyword;
	}
}

static int
kftpd_handle_command (server_parms_t *parms)
{
	char		path[CLIENT_CWD_SIZE], buf[300];
	unsigned int	response = 0;
	int		n, quit = 0;

	n = ksock_rw(parms->clientsock, buf, sizeof(buf), 0);
	if (n < 0) {
		PRINTK("%s: ksock_rw() failed, rc=%d\n", parms->servername, n);
		return -1;
	} else if (n == 0) {
		PRINTK("%s: EOF on client sock\n", parms->servername);
		return -1;
	}
	//PRINTK("%s: '%02x %02x %02x %02x %02x '\n(size=%d)\n", parms->servername, buf[0],buf[1],buf[2],buf[3],buf[4], n);
	if (n >= sizeof(buf))
		n = sizeof(buf) - 1;
	buf[n] = '\0';
	if (buf[n - 1] == '\n')
		buf[--n] = '\0';
	if (buf[n - 1] == '\r')
		buf[--n] = '\0';
	PRINTK("%s: '%s' len=%d\n", parms->servername, buf, n);
	make_keyword_uppercase(buf);
	if (!strcmp(buf, "QUIT")) {
		quit = 1;
		response = 221;
	} else if (!strncmp(buf, "USER ", 5)) {
		response = 230;
	} else if (!strncmp(buf, "PASS ", 5)) {
		response = 202;
	} else if (!strncmp(buf, "SYST", 4)) {
		response = 215;
	} else if (!strcmp(buf, "MODE S")) {
		response = 200;
	} else if (!strcmp(buf, "STRU F")) {
		response = 200;
	} else if (!strncmp(buf, "TYPE ", 5)) {
		if (buf[5] != 'I') {
			//response = 502;
			response = 200;
		} else {
			response = 200;
		}
	} else if (!strncmp(buf, "CWD ",4)) {
		append_path(parms->cwd, &buf[4]);
		quit = send_dir_response(parms, 250, parms->cwd, "directory changed");
	} else if (!strcmp(buf, "CDUP")) {
		append_path(parms->cwd, "..");
		quit = send_dir_response(parms, 200, parms->cwd, NULL);
	} else if (!strcmp(buf, "NOOP")) {
		response = 200;
	} else if (!strcmp(buf, "HELP")) {
		quit = send_help_response(parms, "", "   USER    PORT    STOR    NLST    MKD     CDUP    PASS    ABOR*   \r\n   SITE    TYPE*   DELE    SYST*   RMD     STRU*   CWD     MODE*   \r\n   HELP    PWD     QUIT    RETR    LIST    NOOP    \r\n");
		response = 214;
	} else if (!strcmp(buf, "PWD")) {
		quit = send_dir_response(parms, 257, parms->cwd, NULL);
	} else if (!strcmp(buf, "SITE HELP")) {
		quit = send_help_response(parms, "SITE ", "   CHMOD   HELP    \r\n");
		response = 214;
	} else if (!strncmp(buf, "SITE CHMOD 0", 12)) {
		unsigned char *p = &buf[12];
		unsigned int mode = 0;
		while (*p >= '0' && *p <= '7')
			mode = (mode * 8) + (*p++ - '0');
		if (*p++ != ' ' || !*p) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, p);
			response = do_chmod(parms, mode, path);
		}
	} else if (n == 2 && buf[0] == 0xff && buf[1] == 0xf4) {
		response = 0;	// Ignore the telnet escape sequence
	} else if (n == 5 && buf[0] == 0xf2 && !strcmp(buf+1, "ABOR")) {
		response = 226;
	} else if (!strcmp(buf, "ABOR")) {
		response = 226;
	//} else if (!strncmp(buf, "SITE EXEC ", 10)) {
	//	response = 502;
	} else if (!strncmp(buf, "PORT ", 5)) {
		parms->have_portaddr = 0;
		if (extract_portaddr(&parms->portaddr, &buf[5])) {
			response = 501;
		} else {
			parms->have_portaddr = 1;
			response = 200;
		}
	} else if (!strncmp(buf, "MKD ", 4)) {
		if (!buf[4]) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, &buf[4]);
			response = do_mkdir(parms, path);
		}
	} else if (!strncmp(buf, "RMD ", 4)) {
		if (!buf[4]) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, &buf[4]);
			response = do_rmdir(parms, path);
		}
	} else if (!strncmp(buf, "DELE ", 5)) {
		if (!buf[5]) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, &buf[5]);
			response = do_delete(parms, path);
		}
	} else if (!strncmp(buf, "LIST", 4) || !strncmp(buf, "NLST", 4)) {
		int j = 4;
		if (buf[j] == ' ' && buf[j+1] == '-')
			while (buf[++j] && buf[j] != ' ');
		if (buf[j] && buf[j] != ' ') {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			if (buf[j]) {
				buf[j] = '\0';
				append_path(path, &buf[j+1]);
			}
			response = send_dirlist(parms, path, buf[0] == 'L');
			if (!response)
				response = 226;
		}
	} else if (!strncmp(buf, "RETR ", 5) || !strncmp(buf, "STOR ", 5)) {
		if (!buf[5]) {
			response = 501;
		} else {
			strcpy(path, parms->cwd);
			append_path(path, &buf[5]);
			if (buf[0] == 'R')
				response = send_file(parms, path);
			else
				response = receive_file(parms, path);
			if (!response)
				response = 226;
		}
	} else {
		response = 500;
	}
	if (response)
		send_response(parms, response);
	return quit;
}

static void
khttpd_bad_request (server_parms_t *parms, int rcode, const char *title, const char *text)
{
	static const char kttpd_response[] =
		"<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n"
		"<HTML><HEAD>\n"
		"<TITLE>%d %s</TITLE>\n"
		"</HEAD><BODY>\n"
		"<H1>%s</H1>\n"
		"%s<P>\n"
		"</BODY></HTML>\n";
	char		buf[256];
	unsigned int	len, rc;

	len = sprintf(buf, kttpd_response, rcode, title, title, text);
	rc = ksock_rw(parms->clientsock, buf, len, -1);
	if (rc != len)
		PRINTK("%s: bad_request(): ksock_rw(%d) returned %d\n", parms->servername, len, rc);
}

static int
khttpd_handle_connection (server_parms_t *parms)
{
	const char	GET[4] = "GET ";
	char		*buf = parms->cwd;
	int		response = 0, buflen = CLIENT_CWD_SIZE, n;

	n = ksock_rw(parms->clientsock, buf, buflen, 0);
	if (n < 0) {
		PRINTK("%s: client request too short, ksock_rw() failed: %d\n", parms->servername, n);
		return -1;
	} else if (n == 0) {
		PRINTK("%s: EOF on client sock\n", parms->servername);
		return -1;
	}
	while (--n && (buf[n] == '\n' || buf[n] == '\r'))
		buf[n] = '\0';
	if (n < sizeof(GET) || n >= buflen || strncmp(buf, GET, sizeof(GET)) || !buf[sizeof(GET)]) {
		khttpd_bad_request(parms, 400, "Bad command", "server only supports GET");
	} else {
		unsigned char *path = &buf[sizeof(GET)];
		for (n = 0; path[n] && path[n] != ' '; ++n); // ignore and strip off all the other http parameters
		path[n--] = '\0';
		PRINTK("%s: '%s'\n", parms->servername, buf);
		// fixme? (maybe):  need to translate incoming char sequences of "%2F" to slashes
		if (path[n] == '/') {
			while (n > 0 && path[n] == '/')
				path[n--] = '\0';
			response = send_dirlist(parms, path, 1);
		} else {
			response = send_file(parms, path);
		}
		if (response) {
			sprintf(buf, "(%d)", response);
			khttpd_bad_request(parms, 404, "Error retrieving file", buf);
		}
	}
	return 0;
}

static int
get_clientip (server_parms_t *parms)
{
	int			addrlen = sizeof(struct sockaddr_in);

	parms->clientip[0] = '\0';
	return (parms->clientsock->ops->getname(parms->clientsock, (struct sockaddr *)&parms->clientaddr, &addrlen, 1) < 0)
	 	|| (parms->clientaddr.sin_family != AF_INET)
	 	|| !inet_ntop(AF_INET, &parms->clientaddr.sin_addr.s_addr, parms->clientip, INET_ADDRSTRLEN);
}

static int
ksock_accept (server_parms_t *parms)
{
	lock_kernel();
	if ((parms->clientsock = sock_alloc())) {
		parms->clientsock->type = parms->servsock->type;
		if (parms->servsock->ops->dup(parms->clientsock, parms->servsock) < 0) {
			PRINTK("%s: sock_accept: dup() failed\n", parms->servername);
		} else if (parms->clientsock->ops->accept(parms->servsock, parms->clientsock, parms->servsock->file->f_flags) < 0) {
			PRINTK("%s: sock_accept: accept() failed\n", parms->servername);
		} else {
			unlock_kernel();
			return 0;	// success
		}
		sock_release(parms->clientsock);
	}
	unlock_kernel();
	return 1;	// failure
}

static void
run_server (int use_http, struct semaphore *sem_p)
{
	mm_segment_t		old_fs;
	struct task_struct	*tsk = current;
	server_parms_t		*parms = kmalloc(sizeof(server_parms_t), GFP_KERNEL);
	int			control_port;

	if (!parms)
		return;
	memset(parms, 0, sizeof(parms));
	parms->use_http  = use_http;
	strcpy(parms->servername, use_http ? "khttpd" : "kftpd");

	// kthread setup
	tsk->session = 1;
	tsk->pgrp = 1;
	strcpy(tsk->comm, parms->servername);
	sigfillset(&tsk->blocked);

	if (sem_p) {
		*sem_p = MUTEX_LOCKED;
		down(sem_p);	// wait for hijack.c to get our port numbers from config.ini
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	if (use_http) {
		control_port = hijack_khttpd_port;
		parms->verbose = hijack_khttpd_verbose;
	} else {
		control_port = hijack_kftpd_control_port;
		parms->data_port = hijack_kftpd_data_port;
		parms->verbose = hijack_kftpd_verbose;
	}
	if (control_port) {
		if (make_socket(parms, &parms->servsock, control_port)) {
			PRINTK("%s: make_socket(port=%d) failed\n", parms->servername, control_port);
		} else if (parms->servsock->ops->listen(parms->servsock, use_http ? 5 : 1) < 0) {
			PRINTK("%s: listen(port=%d) failed\n", parms->servername, control_port);
		} else {
			printk("%s: listening on port %d\n", parms->servername, control_port);
			while (1) {
				if (ksock_accept(parms)) {
					PRINTK("%s: accept() failed\n", parms->servername);
				} else {
					if (get_clientip(parms)) {
						PRINTK("%s: get_clientip failed\n", parms->servername);
					} else {
						PRINTK("%s: connection from %s\n", parms->servername, parms->clientip);
						if (parms->use_http) {
							khttpd_handle_connection(parms);
						} else if (!send_response(parms, 220)) {
							strcpy(parms->cwd, "/");
							parms->umask = 0022;
							while (!kftpd_handle_command(parms));
						}
					}
					sock_release(parms->clientsock);
				}
			}
		}
	}
	set_fs(old_fs);
}

int kftpd (void *unused)	// invoked from init/main.c
{
	run_server(0, &hijack_kftpd_startup_sem);
	return 0;
}

int khttpd (void *unused)	// invoked from init/main.c
{
	run_server(1, &hijack_khttpd_startup_sem);
	return 0;
}

