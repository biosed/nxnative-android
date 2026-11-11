// Darwin's xattr calls on Linux (see ../README.md): the position / options arguments go, XATTR_NOFOLLOW picks the l* variant; ENOATTR is ENODATA
#pragma once
#include_next <sys/xattr.h>
#include <errno.h>
#define XATTR_NOFOLLOW 1
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
#define getxattr(p, n, v, s, pos, opt) ((opt) & XATTR_NOFOLLOW ? lgetxattr(p, n, v, s) : getxattr(p, n, v, s))
#define setxattr(p, n, v, s, pos, opt) ((opt) & XATTR_NOFOLLOW ? lsetxattr(p, n, v, s, 0) : setxattr(p, n, v, s, 0))
#define removexattr(p, n, opt) ((opt) & XATTR_NOFOLLOW ? lremovexattr(p, n) : removexattr(p, n))
#define listxattr(p, b, s, opt) ((opt) & XATTR_NOFOLLOW ? llistxattr(p, b, s) : listxattr(p, b, s))
