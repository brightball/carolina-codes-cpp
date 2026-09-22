#ifndef CAROLINA_CPP_H
#define CAROLINA_CPP_H

#include <cstddef>
#if defined(__has_include)
#if __has_include(<postgresql/libpq-fe.h>)
#include <postgresql/libpq-fe.h>
#else
#include <libpq-fe.h>
#endif
#else
#include <libpq-fe.h>
#endif

void carolina_init();
void carolina_reset_counts();
int carolina_sql_count();
int carolina_connect_count();
void carolina_set_connect_fn(PGconn *(*fn)(const char *));
PGconn *carolina_db_acquire();
void carolina_db_release(PGconn *c);
int carolina_listen_family();
int carolina_handle_health_copy(char *buf, size_t buflen);
int carolina_handle_speakers_year(int year, char *buf, size_t buflen);
int carolina_bind_ipv6();
int carolina_serve_start();
void carolina_serve_stop();

#endif
