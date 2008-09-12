/*
 * Copyright (C) 2000, 2001, 2004, 2005, 2006  Free Software Foundation
 *
 * Author: Nikos Mavrogiannopoulos
 *
 * This file is part of GNUTLS.
 *
 * The GNUTLS library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,
 * USA
 *
 */

/* This file contains functions which are wrappers for the key exchange
 * part of TLS. They are called by the handshake functions (MHD_gnutls_handshake)
 */

#include "gnutls_int.h"
#include "gnutls_handshake.h"
#include "gnutls_kx.h"
#include "gnutls_dh.h"
#include "gnutls_errors.h"
#include "gnutls_algorithms.h"
#include "debug.h"
#include "gnutls_mpi.h"
#include <gnutls_state.h>
#include <gnutls_datum.h>
#include <gnutls_rsa_export.h>

/* This file contains important thing for the TLS handshake procedure.
 */

#define MASTER_SECRET "master secret"
static int generate_normal_master (mhd_gtls_session_t session, int);

int
mhd_gtls_generate_master (mhd_gtls_session_t session, int keep_premaster)
{
  if (session->internals.resumed == RESUME_FALSE)
    return generate_normal_master (session, keep_premaster);
  return 0;
}

/* here we generate the TLS Master secret.
 */
#define PREMASTER session->key->key
static int
generate_normal_master (mhd_gtls_session_t session, int keep_premaster)
{
  int ret = 0;
  char buf[512];

  _gnutls_hard_log ("INT: PREMASTER SECRET[%d]: %s\n", PREMASTER.size,
                    mhd_gtls_bin2hex (PREMASTER.data, PREMASTER.size, buf,
                                      sizeof (buf)));
  _gnutls_hard_log ("INT: CLIENT RANDOM[%d]: %s\n", 32,
                    mhd_gtls_bin2hex (session->security_parameters.
                                      client_random, 32, buf, sizeof (buf)));
  _gnutls_hard_log ("INT: SERVER RANDOM[%d]: %s\n", 32,
                    mhd_gtls_bin2hex (session->security_parameters.
                                      server_random, 32, buf, sizeof (buf)));

  if (MHD_gnutls_protocol_get_version (session) == MHD_GNUTLS_PROTOCOL_SSL3)
    {
      opaque rnd[2 * TLS_RANDOM_SIZE + 1];

      memcpy (rnd, session->security_parameters.client_random,
              TLS_RANDOM_SIZE);
      memcpy (&rnd[TLS_RANDOM_SIZE],
              session->security_parameters.server_random, TLS_RANDOM_SIZE);

      ret =
        mhd_gnutls_ssl3_generate_random (PREMASTER.data, PREMASTER.size,
                                         rnd, 2 * TLS_RANDOM_SIZE,
                                         TLS_MASTER_SIZE,
                                         session->security_parameters.
                                         master_secret);

    }
  else if (session->security_parameters.extensions.oprfi_client_len > 0 &&
           session->security_parameters.extensions.oprfi_server_len > 0)
    {
      opaque *rnd;
      size_t rndlen = 2 * TLS_RANDOM_SIZE;

      rndlen += session->security_parameters.extensions.oprfi_client_len;
      rndlen += session->security_parameters.extensions.oprfi_server_len;

      rnd = gnutls_malloc (rndlen + 1);
      if (!rnd)
        {
          gnutls_assert ();
          return GNUTLS_E_MEMORY_ERROR;
        }

      _gnutls_hard_log ("INT: CLIENT OPRFI[%d]: %s\n",
                        session->security_parameters.
                        extensions.oprfi_server_len,
                        mhd_gtls_bin2hex (session->security_parameters.
                                          extensions.oprfi_client,
                                          session->security_parameters.
                                          extensions.oprfi_client_len, buf,
                                          sizeof (buf)));
      _gnutls_hard_log ("INT: SERVER OPRFI[%d]: %s\n",
                        session->security_parameters.
                        extensions.oprfi_server_len,
                        mhd_gtls_bin2hex (session->security_parameters.
                                          extensions.oprfi_server,
                                          session->security_parameters.
                                          extensions.oprfi_server_len, buf,
                                          sizeof (buf)));

      memcpy (rnd, session->security_parameters.client_random,
              TLS_RANDOM_SIZE);
      memcpy (rnd + TLS_RANDOM_SIZE,
              session->security_parameters.extensions.oprfi_client,
              session->security_parameters.extensions.oprfi_client_len);
      memcpy (rnd + TLS_RANDOM_SIZE +
              session->security_parameters.extensions.oprfi_client_len,
              session->security_parameters.server_random, TLS_RANDOM_SIZE);
      memcpy (rnd + TLS_RANDOM_SIZE +
              session->security_parameters.extensions.oprfi_client_len +
              TLS_RANDOM_SIZE,
              session->security_parameters.extensions.oprfi_server,
              session->security_parameters.extensions.oprfi_server_len);

      ret = mhd_gtls_PRF (session, PREMASTER.data, PREMASTER.size,
                          MASTER_SECRET, strlen (MASTER_SECRET),
                          rnd, rndlen, TLS_MASTER_SIZE,
                          session->security_parameters.master_secret);

      gnutls_free (rnd);
    }
  else
    {
      opaque rnd[2 * TLS_RANDOM_SIZE + 1];

      memcpy (rnd, session->security_parameters.client_random,
              TLS_RANDOM_SIZE);
      memcpy (&rnd[TLS_RANDOM_SIZE],
              session->security_parameters.server_random, TLS_RANDOM_SIZE);

      ret =
        mhd_gtls_PRF (session, PREMASTER.data, PREMASTER.size,
                      MASTER_SECRET, strlen (MASTER_SECRET),
                      rnd, 2 * TLS_RANDOM_SIZE, TLS_MASTER_SIZE,
                      session->security_parameters.master_secret);
    }

  /* TLS/IA inner secret is derived from the master secret. */
  memcpy (session->security_parameters.inner_secret,
          session->security_parameters.master_secret, TLS_MASTER_SIZE);

  if (!keep_premaster)
    _gnutls_free_datum (&PREMASTER);

  if (ret < 0)
    return ret;

  _gnutls_hard_log ("INT: MASTER SECRET: %s\n",
                    mhd_gtls_bin2hex (session->security_parameters.
                                      master_secret, TLS_MASTER_SIZE, buf,
                                      sizeof (buf)));

  return ret;
}


/* This is called when we want to receive the key exchange message of the
 * server. It does nothing if this type of message is not required
 * by the selected ciphersuite.
 */
int
mhd_gtls_send_server_kx_message (mhd_gtls_session_t session, int again)
{
  uint8_t *data = NULL;
  int data_size = 0;
  int ret = 0;

  if (session->internals.auth_struct->mhd_gtls_gen_server_kx == NULL)
    return 0;

  data = NULL;
  data_size = 0;

  if (again == 0)
    {
      data_size =
        session->internals.auth_struct->mhd_gtls_gen_server_kx (session,
                                                                &data);

      if (data_size == GNUTLS_E_INT_RET_0)
        {
          gnutls_assert ();
          return 0;
        }

      if (data_size < 0)
        {
          gnutls_assert ();
          return data_size;
        }
    }

  ret =
    mhd_gtls_send_handshake (session, data, data_size,
                             GNUTLS_HANDSHAKE_SERVER_KEY_EXCHANGE);
  gnutls_free (data);

  if (ret < 0)
    {
      gnutls_assert ();
      return ret;
    }
  return data_size;
}

/* This function sends a certificate request message to the
 * client.
 */
int
mhd_gtls_send_server_certificate_request (mhd_gtls_session_t session,
                                          int again)
{
  uint8_t *data = NULL;
  int data_size = 0;
  int ret = 0;

  if (session->internals.auth_struct->
      mhd_gtls_gen_server_certificate_request == NULL)
    return 0;

  if (session->internals.send_cert_req <= 0)
    return 0;

  data = NULL;
  data_size = 0;

  if (again == 0)
    {
      data_size =
        session->internals.auth_struct->
        mhd_gtls_gen_server_certificate_request (session, &data);

      if (data_size < 0)
        {
          gnutls_assert ();
          return data_size;
        }
    }
  ret =
    mhd_gtls_send_handshake (session, data, data_size,
                             GNUTLS_HANDSHAKE_CERTIFICATE_REQUEST);
  gnutls_free (data);

  if (ret < 0)
    {
      gnutls_assert ();
      return ret;
    }
  return data_size;
}


/* This is the function for the client to send the key
 * exchange message
 */
int
mhd_gtls_send_client_kx_message (mhd_gtls_session_t session, int again)
{
  uint8_t *data;
  int data_size;
  int ret = 0;

  if (session->internals.auth_struct->mhd_gtls_gen_client_kx == NULL)
    return 0;


  data = NULL;
  data_size = 0;

  if (again == 0)
    {
      data_size =
        session->internals.auth_struct->mhd_gtls_gen_client_kx (session,
                                                                &data);
      if (data_size < 0)
        {
          gnutls_assert ();
          return data_size;
        }
    }
  ret =
    mhd_gtls_send_handshake (session, data, data_size,
                             GNUTLS_HANDSHAKE_CLIENT_KEY_EXCHANGE);
  gnutls_free (data);

  if (ret < 0)
    {
      gnutls_assert ();
      return ret;
    }

  return ret;
}


/* This is the function for the client to send the certificate
 * verify message
 */
int
mhd_gtls_send_client_certificate_verify (mhd_gtls_session_t session,
                                         int again)
{
  uint8_t *data;
  int ret = 0;
  int data_size;

  /* This is a packet that is only sent by the client
   */
  if (session->security_parameters.entity == GNUTLS_SERVER)
    return 0;

  /* if certificate verify is not needed just exit
   */
  if (session->key->certificate_requested == 0)
    return 0;

  if (session->internals.auth_struct->mhd_gtls_gen_client_cert_vrfy == NULL)
    {
      gnutls_assert ();
      return 0;                 /* this algorithm does not support cli_cert_vrfy
                                 */
    }

  data = NULL;
  data_size = 0;

  if (again == 0)
    {
      data_size =
        session->internals.auth_struct->
        mhd_gtls_gen_client_cert_vrfy (session, &data);
      if (data_size < 0)
        {
          gnutls_assert ();
          return data_size;
        }
      if (data_size == 0)
        return 0;

    }
  ret =
    mhd_gtls_send_handshake (session, data,
                             data_size, GNUTLS_HANDSHAKE_CERTIFICATE_VERIFY);
  gnutls_free (data);

  return ret;
}


int
mhd_gtls_recv_server_kx_message (mhd_gtls_session_t session)
{
  uint8_t *data = NULL;
  int datasize;
  int ret = 0;

  if (session->internals.auth_struct->mhd_gtls_process_server_kx != NULL)
    {

      /* EXCEPTION FOR RSA_EXPORT cipher suite
       */
      if (mhd_gtls_session_is_export (session) != 0 &&
          _gnutls_peers_cert_less_512 (session) != 0)
        {
          gnutls_assert ();
          return 0;
        }

      ret =
        mhd_gtls_recv_handshake (session, &data,
                                 &datasize,
                                 GNUTLS_HANDSHAKE_SERVER_KEY_EXCHANGE,
                                 MANDATORY_PACKET);
      if (ret < 0)
        {
          gnutls_assert ();
          return ret;
        }

      ret =
        session->internals.auth_struct->mhd_gtls_process_server_kx (session,
                                                                    data,
                                                                    datasize);
      gnutls_free (data);

      if (ret < 0)
        {
          gnutls_assert ();
          return ret;
        }

    }
  return ret;
}

int
mhd_gtls_recv_server_certificate_request (mhd_gtls_session_t session)
{
  uint8_t *data;
  int datasize;
  int ret = 0;

  if (session->internals.auth_struct->
      mhd_gtls_process_server_certificate_request != NULL)
    {

      ret =
        mhd_gtls_recv_handshake (session, &data,
                                 &datasize,
                                 GNUTLS_HANDSHAKE_CERTIFICATE_REQUEST,
                                 OPTIONAL_PACKET);
      if (ret < 0)
        return ret;

      if (ret == 0 && datasize == 0)
        return 0;               /* ignored */

      ret =
        session->internals.auth_struct->
        mhd_gtls_process_server_certificate_request (session, data, datasize);
      gnutls_free (data);
      if (ret < 0)
        return ret;

    }
  return ret;
}

int
mhd_gtls_recv_client_kx_message (mhd_gtls_session_t session)
{
  uint8_t *data;
  int datasize;
  int ret = 0;


  /* Do key exchange only if the algorithm permits it */
  if (session->internals.auth_struct->mhd_gtls_process_client_kx != NULL)
    {

      ret =
        mhd_gtls_recv_handshake (session, &data,
                                 &datasize,
                                 GNUTLS_HANDSHAKE_CLIENT_KEY_EXCHANGE,
                                 MANDATORY_PACKET);
      if (ret < 0)
        return ret;

      ret =
        session->internals.auth_struct->mhd_gtls_process_client_kx (session,
                                                                    data,
                                                                    datasize);
      gnutls_free (data);
      if (ret < 0)
        return ret;

    }

  return ret;
}


/* This is called when we want send our certificate
 */
int
mhd_gtls_send_client_certificate (mhd_gtls_session_t session, int again)
{
  uint8_t *data = NULL;
  int data_size = 0;
  int ret = 0;


  if (session->key->certificate_requested == 0)
    return 0;

  if (session->internals.auth_struct->mhd_gtls_gen_client_certificate == NULL)
    return 0;

  data = NULL;
  data_size = 0;

  if (again == 0)
    {
      if (MHD_gnutls_protocol_get_version (session) != MHD_GNUTLS_PROTOCOL_SSL3 ||
          session->internals.selected_cert_list_length > 0)
        {
          /* TLS 1.0 or SSL 3.0 with a valid certificate
           */
          data_size =
            session->internals.auth_struct->
            mhd_gtls_gen_client_certificate (session, &data);

          if (data_size < 0)
            {
              gnutls_assert ();
              return data_size;
            }
        }
    }

  /* In the SSL 3.0 protocol we need to send a
   * no certificate alert instead of an
   * empty certificate.
   */
  if (MHD_gnutls_protocol_get_version (session) == MHD_GNUTLS_PROTOCOL_SSL3 &&
      session->internals.selected_cert_list_length == 0)
    {
      ret =
        MHD_gnutls_alert_send (session, GNUTLS_AL_WARNING,
                               GNUTLS_A_SSL3_NO_CERTIFICATE);

    }
  else
    {                           /* TLS 1.0 or SSL 3.0 with a valid certificate
                                 */
      ret =
        mhd_gtls_send_handshake (session, data, data_size,
                                 GNUTLS_HANDSHAKE_CERTIFICATE_PKT);
      gnutls_free (data);
    }

  if (ret < 0)
    {
      gnutls_assert ();
      return ret;
    }

  return data_size;
}


/* This is called when we want send our certificate
 */
int
mhd_gtls_send_server_certificate (mhd_gtls_session_t session, int again)
{
  uint8_t *data = NULL;
  int data_size = 0;
  int ret = 0;


  if (session->internals.auth_struct->mhd_gtls_gen_server_certificate == NULL)
    return 0;

  data = NULL;
  data_size = 0;

  if (again == 0)
    {
      data_size =
        session->internals.auth_struct->
        mhd_gtls_gen_server_certificate (session, &data);

      if (data_size < 0)
        {
          gnutls_assert ();
          return data_size;
        }
    }
  ret =
    mhd_gtls_send_handshake (session, data, data_size,
                             GNUTLS_HANDSHAKE_CERTIFICATE_PKT);
  gnutls_free (data);

  if (ret < 0)
    {
      gnutls_assert ();
      return ret;
    }

  return data_size;
}


int
mhd_gtls_recv_client_certificate (mhd_gtls_session_t session)
{
  int datasize;
  opaque *data;
  int ret = 0;
  int optional;

  if (session->internals.auth_struct->mhd_gtls_process_client_certificate !=
      NULL)
    {

      /* if we have not requested a certificate then just return
       */
      if (session->internals.send_cert_req == 0)
        {
          return 0;
        }

      if (session->internals.send_cert_req == GNUTLS_CERT_REQUIRE)
        optional = MANDATORY_PACKET;
      else
        optional = OPTIONAL_PACKET;

      ret =
        mhd_gtls_recv_handshake (session, &data,
                                 &datasize,
                                 GNUTLS_HANDSHAKE_CERTIFICATE_PKT, optional);

      if (ret < 0)
        {
          /* Handle the case of old SSL3 clients who send
           * a warning alert instead of an empty certificate to indicate
           * no certificate.
           */
          if (optional == OPTIONAL_PACKET &&
              ret == GNUTLS_E_WARNING_ALERT_RECEIVED &&
              MHD_gnutls_protocol_get_version (session) == MHD_GNUTLS_PROTOCOL_SSL3 &&
              gnutls_alert_get (session) == GNUTLS_A_SSL3_NO_CERTIFICATE)
            {

              /* SSL3 does not send an empty certificate,
               * but this alert. So we just ignore it.
               */
              gnutls_assert ();
              return 0;
            }

          /* certificate was required
           */
          if ((ret == GNUTLS_E_WARNING_ALERT_RECEIVED
               || ret == GNUTLS_E_FATAL_ALERT_RECEIVED)
              && optional == MANDATORY_PACKET)
            {
              gnutls_assert ();
              return GNUTLS_E_NO_CERTIFICATE_FOUND;
            }

          return ret;
        }

      if (ret == 0 && datasize == 0 && optional == OPTIONAL_PACKET)
        {
          /* Client has not sent the certificate message.
           * well I'm not sure we should accept this
           * behaviour.
           */
          gnutls_assert ();
          return 0;
        }
      ret =
        session->internals.auth_struct->
        mhd_gtls_process_client_certificate (session, data, datasize);

      gnutls_free (data);
      if (ret < 0 && ret != GNUTLS_E_NO_CERTIFICATE_FOUND)
        {
          gnutls_assert ();
          return ret;
        }

      /* ok we should expect a certificate verify message now
       */
      if (ret == GNUTLS_E_NO_CERTIFICATE_FOUND && optional == OPTIONAL_PACKET)
        ret = 0;
      else
        session->key->certificate_requested = 1;

    }

  return ret;
}

int
mhd_gtls_recv_server_certificate (mhd_gtls_session_t session)
{
  int datasize;
  opaque *data;
  int ret = 0;

  if (session->internals.auth_struct->mhd_gtls_process_server_certificate !=
      NULL)
    {

      ret =
        mhd_gtls_recv_handshake (session, &data,
                                 &datasize,
                                 GNUTLS_HANDSHAKE_CERTIFICATE_PKT,
                                 MANDATORY_PACKET);
      if (ret < 0)
        {
          gnutls_assert ();
          return ret;
        }

      ret =
        session->internals.auth_struct->
        mhd_gtls_process_server_certificate (session, data, datasize);
      gnutls_free (data);
      if (ret < 0)
        {
          gnutls_assert ();
          return ret;
        }
    }

  return ret;
}


/* Recv the client certificate verify. This packet may not
 * arrive if the peer did not send us a certificate.
 */
int
mhd_gtls_recv_client_certificate_verify_message (mhd_gtls_session_t session)
{
  uint8_t *data;
  int datasize;
  int ret = 0;


  if (session->internals.auth_struct->mhd_gtls_process_client_cert_vrfy !=
      NULL)
    {

      if (session->internals.send_cert_req == 0 ||
          session->key->certificate_requested == 0)
        {
          return 0;
        }

      ret =
        mhd_gtls_recv_handshake (session, &data,
                                 &datasize,
                                 GNUTLS_HANDSHAKE_CERTIFICATE_VERIFY,
                                 OPTIONAL_PACKET);
      if (ret < 0)
        return ret;

      if (ret == 0 && datasize == 0
          && session->internals.send_cert_req == GNUTLS_CERT_REQUIRE)
        {
          /* certificate was required */
          gnutls_assert ();
          return GNUTLS_E_NO_CERTIFICATE_FOUND;
        }

      ret =
        session->internals.auth_struct->
        mhd_gtls_process_client_cert_vrfy (session, data, datasize);
      gnutls_free (data);
      if (ret < 0)
        return ret;

    }

  return ret;
}
