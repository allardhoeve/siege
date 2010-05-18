/**
 * SSL Thread Safe Setup Functions.
 *
 * Copyright (C) 2002-2007 by
 * Jeffrey Fulmer - <jeff@joedog.org>, et al. 
 * This file is distributed as part of Siege
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * --
 */
#ifdef  HAVE_CONFIG_H
# include <config.h>
#endif/*HAVE_CONFIG_H*/

#include <setup.h>
#include <ssl.h>
#include <util.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

/**
 * local variables and prototypes
 */
#ifdef  HAVE_SSL
static pthread_mutex_t *lock_cs;
static long            *lock_count;
#endif/*HAVE_SSL*/

unsigned long SSL_pthreads_thread_id(void);
static   void SSL_error_stack(void); 
static   void SSL_pthreads_locking_callback(int m, int t, char *f, int l);

BOOLEAN
SSL_initialize(CONN *C)
{
#ifdef HAVE_SSL
  int  i;
  int  serr;
  
  C->ssl    = NULL;
  C->ctx    = NULL;
  C->method = NULL;
  C->cert   = NULL; 
  
  /** 
   * XXX: SSL_library_init(); 
   *      SSL_load_error_strings();
   * moved to ssl.c:235 - use once 
   */
  if(!my.ssl_key && my.ssl_cert) {
    my.ssl_key = my.ssl_cert;
  }
  if(!my.ssl_ciphers) {
    my.ssl_ciphers = stralloc(SSL_DEFAULT_CIPHER_LIST);
  } 

  C->method = SSLv23_client_method();
  if(C->method==NULL){
    SSL_error_stack();
    return FALSE;
  } 
  C->ctx = SSL_CTX_new(C->method);
  if(C->ctx==NULL){
    SSL_error_stack();
    return FALSE;
  } 

  SSL_CTX_set_mode(C->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  SSL_CTX_set_session_cache_mode(C->ctx, SSL_SESS_CACHE_BOTH);
  SSL_CTX_set_timeout(C->ctx, my.ssl_timeout);
  if(my.ssl_ciphers){
    if(!SSL_CTX_set_cipher_list(C->ctx, my.ssl_ciphers)){
      joe_error("SSL_CTX_set_cipher_list");
      return FALSE;
    }
  }

  if(my.ssl_cert){
    if(!SSL_CTX_use_certificate_chain_file(C->ctx, my.ssl_cert)){
      SSL_error_stack(); /* dump the error stack */
      joe_fatal("Error reading certificate file: %s", my.ssl_cert);
    }
    for(i=0; i<3; i++){
      if(SSL_CTX_use_PrivateKey_file(C->ctx, my.ssl_key, SSL_FILETYPE_PEM))
        break;
      if(i<2 && ERR_GET_REASON(ERR_peek_error())==EVP_R_BAD_DECRYPT){
        SSL_error_stack(); /* dump the error stack */
        log_warning("Wrong pass phrase: retrying");
        continue;
      }
    }

    if(!SSL_CTX_check_private_key(C->ctx)){
      joe_error("Private key does not match the certificate");
      return FALSE;
    }
  }  

  C->ssl = SSL_new(C->ctx);
  if(C->ssl==NULL){
    SSL_error_stack();
    return FALSE;
  }
  SSL_set_fd(C->ssl, C->sock);
  serr = SSL_connect(C->ssl);
  return TRUE;
#else
  return FALSE;
#endif/*HAVE_SSL*/
}

/**
 * these functions were more or less taken from
 * the openssl thread safe examples included in
 * the OpenSSL distribution.
 */
#ifdef HAVE_SSL
void 
SSL_thread_setup( void ) 
{
  int x;
 
#define OPENSSL_THREAD_DEFINES
#include <openssl/opensslconf.h>
#if defined(THREADS) || defined(OPENSSL_THREADS)
#else
   fprintf(
    stderr, 
    "WARNING: your openssl libraries were compiled without thread support\n"
   );
   pthread_sleep_np( 2 );
#endif

  SSL_library_init();
  SSL_load_error_strings();
  lock_cs    = (pthread_mutex_t*)OPENSSL_malloc(
    CRYPTO_num_locks()*sizeof(pthread_mutex_t)
  );
  lock_count = (long*)OPENSSL_malloc(
    CRYPTO_num_locks() * sizeof(long)
  );

  for( x = 0; x < CRYPTO_num_locks(); x++ ){
    lock_count[x] = 0;
    pthread_mutex_init(&(lock_cs[x]), NULL);
  }
  CRYPTO_set_id_callback((unsigned long (*)())SSL_pthreads_thread_id);
  CRYPTO_set_locking_callback((void (*)())SSL_pthreads_locking_callback);
}

void 
SSL_thread_cleanup(void) 
{
  int x;
  
  CRYPTO_set_locking_callback(NULL);
  for(x = 0; x < CRYPTO_num_locks(); x++){
    pthread_mutex_destroy(&(lock_cs[x]));
  }
  if(lock_cs!=(pthread_mutex_t *)NULL) { 
    OPENSSL_free(lock_cs); 
    lock_cs=(pthread_mutex_t *)NULL; 
  }
  if(lock_count!=(long *)NULL){ 
    OPENSSL_free(lock_count); 
    lock_count=(long *)NULL; 
  }
}

void 
SSL_pthreads_locking_callback(int mode, int type, char *file, int line) 
{
  if( my.debug == 4 ){
    fprintf(
      stderr,"thread=%4d mode=%s lock=%s %s:%d\n", (int)CRYPTO_thread_id(),
      (mode&CRYPTO_LOCK)?"l":"u", (type&CRYPTO_READ)?"r":"w",file,line
    );
  }
  if(mode & CRYPTO_LOCK){
    pthread_mutex_lock(&(lock_cs[type]));
    lock_count[type]++;
  } 
  else{ 
    pthread_mutex_unlock(&(lock_cs[type]));
  }
}

unsigned long 
SSL_pthreads_thread_id(void) 
{
  unsigned long ret;
  ret = (unsigned long)pthread_self();

  return(ret);
}

static void 
SSL_error_stack(void) { /* recursive dump of the error stack */
  unsigned long err;
  char string[120];

  err=ERR_get_error();
  if(!err)
    return;
  SSL_error_stack();
  ERR_error_string(err, string);
  joe_error("error stack: %lX : %s", err, string);
} 

#endif/*HAVE_SSL*/
