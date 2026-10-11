/* pool.h - see pool.c: a parallel for over the compositor's columns. */
#pragma once
void pool_for(int n, void (*fn)(int i, void *ctx), void *ctx);
