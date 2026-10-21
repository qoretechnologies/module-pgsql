#ifndef _CONFIG_H
#define _CONFIG_H

/* The symbols in arpa/inet.h and netinet/in.h conflicts with the 
   symbols that the postgresql server headers provide. */ 
#ifndef POSTGRESQL_SERVER_INCLUDES
#cmakedefine HAVE_ARPA_INET_H
#cmakedefine HAVE_NETINET_IN_H
#endif
#cmakedefine HAVE_PQLIBVERSION
#cmakedefine HAVE_PQCONNINFO
#cmakedefine HAVE_PQHOSTADDR
#cmakedefine HAVE_GCC_VISIBILITY

#endif
