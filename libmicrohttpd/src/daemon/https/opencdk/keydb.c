/* keydb.c - Key database routines
 *        Copyright (C) 2002, 2003, 2007 Timo Schulz
 *
 * This file is part of OpenCDK.
 *
 * OpenCDK is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * OpenCDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include "opencdk.h"
#include "main.h"
#include "packet.h"
#include "filters.h"
#include "stream.h"


#define KEYID_CMP(a, b) ((a[0]) == (b[0]) && (a[1]) == (b[1]))
#define KEYDB_CACHE_ENTRIES 8


/* Internal key index structure. */
struct key_idx_s
{
  off_t offset;
  u32 keyid[2];
  byte fpr[KEY_FPR_LEN];
};
typedef struct key_idx_s *key_idx_t;


/* Internal handle for the search operation. */
struct cdk_dbsearch_s
{
  union
  {
    char *pattern;              /* A search is performed by pattern. */
    u32 keyid[2];               /* A search by keyid. */
    byte fpr[KEY_FPR_LEN];      /* A search by fingerprint. */
  } u;
  int type;
};
typedef struct cdk_dbsearch_s *cdk_dbsearch_t;

/* Internal key cache to associate a key with an file offset. */
struct key_table_s
{
  struct key_table_s *next;
  off_t offset;
  cdk_dbsearch_t desc;
};
typedef struct key_table_s *key_table_t;

/* Internal key database handle. */
struct cdk_keydb_hd_s
{
  int type;                     /* type of the key db handle. */
  int fp_ref;                   /* 1=means it is a reference and shall not be closed. */
  cdk_stream_t fp;
  cdk_stream_t idx;
  cdk_dbsearch_t dbs;
  char *name;                   /* name of the underlying file or NULL. */
  char *idx_name;               /* name of the index file or NULL. */
  struct key_table_s *cache;
  size_t ncache;
  unsigned int secret:1;        /* contain secret keys. */
  unsigned int isopen:1;        /* the underlying stream is opened. */
  unsigned int no_cache:1;      /* disable the index cache. */
  unsigned int search:1;        /* handle is in search mode. */

  /* structure to store some stats about the keydb. */
  struct
  {
    size_t new_keys;            /* amount of new keys that were imported. */
  } stats;
};


static void keydb_cache_free (key_table_t cache);
static int keydb_search_copy (cdk_dbsearch_t * r_dst, cdk_dbsearch_t src);
static void keydb_search_free (cdk_dbsearch_t dbs);
static int classify_data (const byte * buf, size_t len);
static cdk_kbnode_t find_selfsig_node (cdk_kbnode_t key, cdk_pkt_pubkey_t pk);


static char *
keydb_idx_mkname (const char *file)
{
  char *fname, *fmt;

  fmt = "%s.idx";
  fname = cdk_calloc (1, strlen (file) + strlen (fmt) + 1);
  if (!fname)
    return NULL;
  sprintf (fname, fmt, file);
  return fname;
}


/* This functions builds an index of the keyring into a separate file
   with the name keyring.ext.idx. It contains the offset of all public-
   and public subkeys. The format of the file is:
   --------
    4 octets offset of the packet
    8 octets keyid
   20 octets fingerprint
   --------
   We store the keyid and the fingerprint due to the fact we can't get
   the keyid from a v3 fingerprint directly.
*/
static cdk_error_t
keydb_idx_build (const char *file)
{
  cdk_packet_t pkt;
  cdk_stream_t inp, out = NULL;
  byte buf[4 + 8 + KEY_FPR_LEN];
  char *idx_name;
  u32 keyid[2];
  cdk_error_t rc;

  if (!file)
    return CDK_Inv_Value;

  rc = cdk_stream_open (file, &inp);
  if (rc)
    return rc;

  idx_name = keydb_idx_mkname (file);
  if (!idx_name)
    {
      cdk_stream_close (inp);
      return CDK_Out_Of_Core;
    }
  rc = cdk_stream_create (idx_name, &out);
  cdk_free (idx_name);
  if (rc)
    {
      cdk_stream_close (inp);
      return rc;
    }

  cdk_pkt_new (&pkt);
  while (!cdk_stream_eof (inp))
    {
      off_t pos = cdk_stream_tell (inp);

      rc = cdk_pkt_read (inp, pkt);
      if (rc)
        {
          _cdk_log_debug ("index build failed packet off=%lu\n", pos);
          /* FIXME: The index is incomplete */
          break;
        }
      if (pkt->pkttype == CDK_PKT_PUBLIC_KEY ||
          pkt->pkttype == CDK_PKT_PUBLIC_SUBKEY)
        {
          _cdk_u32tobuf (pos, buf);
          cdk_pk_get_keyid (pkt->pkt.public_key, keyid);
          _cdk_u32tobuf (keyid[0], buf + 4);
          _cdk_u32tobuf (keyid[1], buf + 8);
          cdk_pk_get_fingerprint (pkt->pkt.public_key, buf + 12);
          cdk_stream_write (out, buf, 4 + 8 + KEY_FPR_LEN);
        }
      cdk_pkt_free (pkt);
    }

  cdk_pkt_release (pkt);

  cdk_stream_close (out);
  cdk_stream_close (inp);
  return rc;
}


/**
 * cdk_keydb_idx_rebuild:
 * @hd: key database handle
 *
 * Rebuild the key index files for the given key database.
 **/
cdk_error_t
cdk_keydb_idx_rebuild (cdk_keydb_hd_t db)
{
  struct stat stbuf;
  char *tmp_idx_name;
  cdk_error_t rc;
  int err;

  if (!db || !db->name)
    return CDK_Inv_Value;
  if (db->secret)
    return 0;

  tmp_idx_name = keydb_idx_mkname (db->name);
  if (!tmp_idx_name)
    return CDK_Out_Of_Core;
  err = stat (tmp_idx_name, &stbuf);
  cdk_free (tmp_idx_name);
  /* This function expects an existing index which can be rebuild,
     if no index exists we do not build one and just return. */
  if (err)
    return 0;

  cdk_stream_close (db->idx);
  db->idx = NULL;
  if (!db->idx_name)
    {
      db->idx_name = keydb_idx_mkname (db->name);
      if (!db->idx_name)
        return CDK_Out_Of_Core;
    }
  rc = keydb_idx_build (db->name);
  if (!rc)
    rc = cdk_stream_open (db->idx_name, &db->idx);
  return rc;
}


static cdk_error_t
keydb_idx_parse (cdk_stream_t inp, key_idx_t * r_idx)
{
  key_idx_t idx;
  byte buf[4];

  if (!inp || !r_idx)
    return CDK_Inv_Value;

  idx = cdk_calloc (1, sizeof *idx);
  if (!idx)
    return CDK_Out_Of_Core;

  while (!cdk_stream_eof (inp))
    {
      if (cdk_stream_read (inp, buf, 4) == CDK_EOF)
        break;
      idx->offset = _cdk_buftou32 (buf);
      cdk_stream_read (inp, buf, 4);
      idx->keyid[0] = _cdk_buftou32 (buf);
      cdk_stream_read (inp, buf, 4);
      idx->keyid[1] = _cdk_buftou32 (buf);
      cdk_stream_read (inp, idx->fpr, KEY_FPR_LEN);
      break;
    }
  *r_idx = idx;
  return cdk_stream_eof (inp) ? CDK_EOF : 0;
}


static cdk_error_t
keydb_idx_search (cdk_stream_t inp, u32 * keyid, const byte * fpr,
                  off_t * r_off)
{
  key_idx_t idx;

  if (!inp || !r_off)
    return CDK_Inv_Value;
  if ((keyid && fpr) || (!keyid && !fpr))
    return CDK_Inv_Mode;

  /* We need an initialize the offset var with a value
     because it might be possible the returned offset will
     be 0 and then we cannot differ between the begin and an EOF. */
  *r_off = 0xFFFFFFFF;
  cdk_stream_seek (inp, 0);
  while (keydb_idx_parse (inp, &idx) != CDK_EOF)
    {
      if (keyid && KEYID_CMP (keyid, idx->keyid))
        {
          *r_off = idx->offset;
          break;
        }
      else if (fpr && !memcmp (idx->fpr, fpr, KEY_FPR_LEN))
        {
          *r_off = idx->offset;
          break;
        }
      cdk_free (idx);
      idx = NULL;
    }
  cdk_free (idx);
  return *r_off != 0xFFFFFFFF ? 0 : CDK_EOF;
}


/**
 * cdk_keydb_new_from_mem:
 * @r_hd: The keydb output handle.
 * @data: The raw key data.
 * @datlen: The length of the raw data.
 * 
 * Create a new keyring db handle from the contents of a buffer.
 */
cdk_error_t
cdk_keydb_new_from_mem (cdk_keydb_hd_t * r_db, int secret,
                        const void *data, size_t datlen)
{
  cdk_keydb_hd_t db;
  cdk_error_t rc;

  if (!r_db)
    return CDK_Inv_Value;
  *r_db = NULL;
  db = calloc (1, sizeof *db);
  rc = cdk_stream_tmp_from_mem (data, datlen, &db->fp);
  if (!db->fp)
    {
      cdk_free (db);
      return rc;
    }
  if (cdk_armor_filter_use (db->fp))
    cdk_stream_set_armor_flag (db->fp, 0);
  db->type = CDK_DBTYPE_DATA;
  db->secret = secret;
  *r_db = db;
  return 0;
}


/**
 * cdk_keydb_new_from_stream:
 * @r_hd: the output keydb handle
 * @secret: does the stream contain secret key data
 * @in: the input stream to use
 * 
 * This function creates a new keydb handle based on the given
 * stream. The stream is not closed in cdk_keydb_free() and it
 * is up to the caller to close it. No decoding is done.
 */
cdk_error_t
cdk_keydb_new_from_stream (cdk_keydb_hd_t * r_hd, int secret, cdk_stream_t in)
{
  cdk_keydb_hd_t hd;

  if (!r_hd)
    return CDK_Inv_Value;
  *r_hd = NULL;

  hd = calloc (1, sizeof *hd);
  hd->fp = in;
  hd->fp_ref = 1;
  hd->type = CDK_DBTYPE_STREAM;
  hd->secret = secret;
  *r_hd = hd;

  /* We do not push any filters and thus we expect that the format
     of the stream has the format the user wanted. */

  return 0;
}


cdk_error_t
cdk_keydb_new_from_file (cdk_keydb_hd_t * r_hd, int secret, const char *fname)
{
  cdk_keydb_hd_t hd;

  if (!r_hd)
    return CDK_Inv_Value;
  *r_hd = NULL;
  hd = calloc (1, sizeof *hd);
  hd->name = cdk_strdup (fname);
  if (!hd->name)
    {
      cdk_free (hd);
      return CDK_Out_Of_Core;
    }
  hd->type = secret ? CDK_DBTYPE_SK_KEYRING : CDK_DBTYPE_PK_KEYRING;
  hd->secret = secret;
  *r_hd = hd;
  return 0;
}



/**
 * cdk_keydb_new:
 * @r_hd: handle to store the new keydb object
 * @type: type of the keyring
 * @data: data which depends on the keyring type
 * @count: length of the data
 *
 * Create a new keydb object
 **/
cdk_error_t
cdk_keydb_new (cdk_keydb_hd_t * r_hd, int type, void *data, size_t count)
{
  switch (type)
    {
    case CDK_DBTYPE_PK_KEYRING:
    case CDK_DBTYPE_SK_KEYRING:
      return cdk_keydb_new_from_file (r_hd, type == CDK_DBTYPE_SK_KEYRING,
                                      (const char *) data);

    case CDK_DBTYPE_DATA:
      return cdk_keydb_new_from_mem (r_hd, 0, data, count);

    case CDK_DBTYPE_STREAM:
      return cdk_keydb_new_from_stream (r_hd, 0, (cdk_stream_t) data);

    default:
      return CDK_Inv_Mode;
    }
  return CDK_Inv_Mode;
}


/**
 * cdk_keydb_free:
 * @hd: the keydb object
 *
 * Free the keydb object.
 **/
void
cdk_keydb_free (cdk_keydb_hd_t hd)
{
  if (!hd)
    return;

  if (hd->name)
    {
      cdk_free (hd->name);
      hd->name = NULL;
    }

  if (hd->fp && !hd->fp_ref)
    {
      cdk_stream_close (hd->fp);
      hd->fp = NULL;
    }

  if (hd->idx)
    {
      cdk_stream_close (hd->idx);
      hd->idx = NULL;
    }

  hd->isopen = 0;
  hd->no_cache = 0;
  hd->secret = 0;
  keydb_cache_free (hd->cache);
  hd->cache = NULL;
  keydb_search_free (hd->dbs);
  hd->dbs = NULL;
  cdk_free (hd);
}


cdk_error_t
_cdk_keydb_open (cdk_keydb_hd_t hd, cdk_stream_t * ret_kr)
{
  cdk_error_t rc, ec;

  if (!hd || !ret_kr)
    return CDK_Inv_Value;

  rc = 0;
  if ((hd->type == CDK_DBTYPE_DATA || hd->type == CDK_DBTYPE_STREAM)
      && hd->fp)
    cdk_stream_seek (hd->fp, 0);
  else if (hd->type == CDK_DBTYPE_PK_KEYRING ||
           hd->type == CDK_DBTYPE_SK_KEYRING)
    {
      if (!hd->isopen && hd->name)
        {
          rc = cdk_stream_open (hd->name, &hd->fp);
          if (rc)
            goto leave;
          if (cdk_armor_filter_use (hd->fp))
            cdk_stream_set_armor_flag (hd->fp, 0);
          hd->isopen = 1;
          /* We disable the index cache for smaller keyrings. */
          if (cdk_stream_get_length (hd->fp) < 524288)
            {
              hd->no_cache = 1;
              goto leave;
            }
          cdk_free (hd->idx_name);
          hd->idx_name = keydb_idx_mkname (hd->name);
          if (!hd->idx_name)
            {
              rc = CDK_Out_Of_Core;
              goto leave;
            }
          ec = cdk_stream_open (hd->idx_name, &hd->idx);
          if (ec && !hd->secret)
            {
              rc = keydb_idx_build (hd->name);
              if (!rc)
                rc = cdk_stream_open (hd->idx_name, &hd->idx);
              if (!rc)
                _cdk_log_debug ("create key index table\n");
              else
                {
                  /* This is no real error, it just means we can't create
                     the index at the given directory. maybe we've no write
                     access. in this case, we simply disable the index. */
                  _cdk_log_debug ("disable key index table err=%d\n", rc);
                  rc = 0;
                  hd->no_cache = 1;
                }
            }
        }
      else
        {
          /* We use the cache to search keys, so we always rewind the
             STREAM. Except when the _NEXT search mode is used because
             this mode is an enumeration and no seeking is needed. */
          if (!hd->search ||
              (hd->search && hd->dbs->type != CDK_DBSEARCH_NEXT))
            cdk_stream_seek (hd->fp, 0);
        }
    }
  else
    return CDK_Inv_Mode;

leave:
  if (rc)
    {
      cdk_stream_close (hd->fp);
      hd->fp = NULL;
    }
  *ret_kr = hd->fp;
  return rc;
}


static int
find_by_keyid (cdk_kbnode_t knode, cdk_dbsearch_t ks)
{
  cdk_kbnode_t node;
  u32 keyid[2];

  for (node = knode; node; node = node->next)
    {
      if (node->pkt->pkttype == CDK_PKT_PUBLIC_KEY ||
          node->pkt->pkttype == CDK_PKT_PUBLIC_SUBKEY ||
          node->pkt->pkttype == CDK_PKT_SECRET_KEY ||
          node->pkt->pkttype == CDK_PKT_SECRET_SUBKEY)
        {
          _cdk_pkt_get_keyid (node->pkt, keyid);
          switch (ks->type)
            {
            case CDK_DBSEARCH_SHORT_KEYID:
              if (keyid[1] == ks->u.keyid[1])
                return 1;
              break;

            case CDK_DBSEARCH_KEYID:
              if (KEYID_CMP (keyid, ks->u.keyid))
                return 1;
              break;

            default:
              _cdk_log_debug ("find_by_keyid: invalid mode = %d\n", ks->type);
              return 0;
            }
        }
    }
  return 0;
}


static int
find_by_fpr (cdk_kbnode_t knode, cdk_dbsearch_t ks)
{
  cdk_kbnode_t node;
  byte fpr[KEY_FPR_LEN];

  if (ks->type != CDK_DBSEARCH_FPR)
    return 0;

  for (node = knode; node; node = node->next)
    {
      if (node->pkt->pkttype == CDK_PKT_PUBLIC_KEY ||
          node->pkt->pkttype == CDK_PKT_PUBLIC_SUBKEY ||
          node->pkt->pkttype == CDK_PKT_SECRET_KEY ||
          node->pkt->pkttype == CDK_PKT_SECRET_SUBKEY)
        {
          _cdk_pkt_get_fingerprint (node->pkt, fpr);
          if (!memcmp (ks->u.fpr, fpr, KEY_FPR_LEN))
            return 1;
          break;
        }
    }

  return 0;
}


static int
find_by_pattern (cdk_kbnode_t knode, cdk_dbsearch_t ks)
{
  cdk_kbnode_t node;
  size_t uidlen;
  char *name;

  for (node = knode; node; node = node->next)
    {
      if (node->pkt->pkttype != CDK_PKT_USER_ID)
        continue;
      if (node->pkt->pkt.user_id->attrib_img != NULL)
        continue;               /* Skip attribute packets. */
      uidlen = node->pkt->pkt.user_id->len;
      name = node->pkt->pkt.user_id->name;
      switch (ks->type)
        {
        case CDK_DBSEARCH_EXACT:
          if (name &&
              (strlen (ks->u.pattern) == uidlen &&
               !strncmp (ks->u.pattern, name, uidlen)))
            return 1;
          break;

        case CDK_DBSEARCH_SUBSTR:
          if (uidlen > 65536)
            break;
          if (name && strlen (ks->u.pattern) > uidlen)
            break;
          if (name && _cdk_memistr (name, uidlen, ks->u.pattern))
            return 1;
          break;

        default:               /* Invalid mode */
          return 0;
        }
    }
  return 0;
}


static void
keydb_search_free (cdk_dbsearch_t dbs)
{
  if (!dbs)
    return;
  if (dbs->type == CDK_DBSEARCH_EXACT || dbs->type == CDK_DBSEARCH_SUBSTR)
    cdk_free (dbs->u.pattern);
  dbs->type = 0;
  cdk_free (dbs);
}


static void
keydb_cache_free (key_table_t cache)
{
  key_table_t c2;

  while (cache)
    {
      c2 = cache->next;
      cache->offset = 0;
      keydb_search_free (cache->desc);
      cdk_free (cache);
      cache = c2;
    }
}


static key_table_t
keydb_cache_find (key_table_t cache, cdk_dbsearch_t desc)
{
  key_table_t t;

  for (t = cache; t; t = t->next)
    {
      if (t->desc->type != desc->type)
        continue;
      switch (t->desc->type)
        {
        case CDK_DBSEARCH_SHORT_KEYID:
        case CDK_DBSEARCH_KEYID:
          if (KEYID_CMP (t->desc->u.keyid, desc->u.keyid))
            return t;
          break;

        case CDK_DBSEARCH_EXACT:
          if (strlen (t->desc->u.pattern) == strlen (desc->u.pattern) &&
              !strcmp (t->desc->u.pattern, desc->u.pattern))
            return t;
          break;

        case CDK_DBSEARCH_SUBSTR:
          if (strstr (t->desc->u.pattern, desc->u.pattern))
            return t;
          break;

        case CDK_DBSEARCH_FPR:
          if (!memcmp (t->desc->u.fpr, desc->u.fpr, KEY_FPR_LEN))
            return t;
          break;
        }
    }

  return NULL;
}


static cdk_error_t
keydb_cache_add (cdk_keydb_hd_t hd, cdk_dbsearch_t dbs, off_t offset)
{
  key_table_t k;

  if (!hd)
    return CDK_Inv_Value;

  if (hd->ncache > KEYDB_CACHE_ENTRIES)
    return 0;                   /* FIXME: we should replace the last entry. */
  k = cdk_calloc (1, sizeof *k);
  if (!k)
    return CDK_Out_Of_Core;
  k->offset = offset;
  keydb_search_copy (&k->desc, dbs);
  k->next = hd->cache;
  hd->cache = k;
  hd->ncache++;
  _cdk_log_debug ("cache: add entry off=%d type=%d\n", offset, dbs->type);
  return 0;
}


static cdk_error_t
keydb_search_copy (cdk_dbsearch_t * r_dst, cdk_dbsearch_t src)
{
  cdk_dbsearch_t dst;

  if (!r_dst || !src)
    return CDK_Inv_Value;

  *r_dst = NULL;
  dst = cdk_calloc (1, sizeof *dst);
  if (!dst)
    return CDK_Out_Of_Core;
  dst->type = src->type;
  switch (src->type)
    {
    case CDK_DBSEARCH_EXACT:
    case CDK_DBSEARCH_SUBSTR:
      dst->u.pattern = cdk_strdup (src->u.pattern);
      if (!dst->u.pattern)
        return CDK_Out_Of_Core;
      break;

    case CDK_DBSEARCH_SHORT_KEYID:
    case CDK_DBSEARCH_KEYID:
      dst->u.keyid[0] = src->u.keyid[0];
      dst->u.keyid[1] = src->u.keyid[1];
      break;

    case CDK_DBSEARCH_FPR:
      memcpy (dst->u.fpr, src->u.fpr, KEY_FPR_LEN);
      break;
    }
  *r_dst = dst;
  return 0;
}


/**
 * cdk_keydb_search_start:
 * @db: key database handle
 * @type: specifies the search type
 * @desc: description which depends on the type
 *
 * Create a new keydb search object.
 **/
cdk_error_t
cdk_keydb_search_start (cdk_keydb_hd_t db, int type, void *desc)
{
  cdk_dbsearch_t dbs;
  u32 *keyid;
  char *p, tmp[3];
  int i;

  if (!db)
    return CDK_Inv_Value;
  if (type != CDK_DBSEARCH_NEXT && !desc)
    return CDK_Inv_Mode;

  dbs = cdk_calloc (1, sizeof *dbs);
  if (!dbs)
    return CDK_Out_Of_Core;
  dbs->type = type;
  switch (type)
    {
    case CDK_DBSEARCH_EXACT:
    case CDK_DBSEARCH_SUBSTR:
      cdk_free (dbs->u.pattern);
      dbs->u.pattern = cdk_strdup (desc);
      if (!dbs->u.pattern)
        {
          cdk_free (dbs);
          return CDK_Out_Of_Core;
        }
      break;

    case CDK_DBSEARCH_SHORT_KEYID:
      keyid = desc;
      dbs->u.keyid[1] = keyid[0];
      break;

    case CDK_DBSEARCH_KEYID:
      keyid = desc;
      dbs->u.keyid[0] = keyid[0];
      dbs->u.keyid[1] = keyid[1];
      break;

    case CDK_DBSEARCH_FPR:
      memcpy (dbs->u.fpr, desc, KEY_FPR_LEN);
      break;

    case CDK_DBSEARCH_NEXT:
      break;

    case CDK_DBSEARCH_AUTO:
      /* Override the type with the actual db search type. */
      dbs->type = classify_data (desc, strlen (desc));
      switch (dbs->type)
        {
        case CDK_DBSEARCH_SUBSTR:
        case CDK_DBSEARCH_EXACT:
          cdk_free (dbs->u.pattern);
          p = dbs->u.pattern = cdk_strdup (desc);
          if (!p)
            {
              cdk_free (dbs);
              return CDK_Out_Of_Core;
            }
          break;

        case CDK_DBSEARCH_SHORT_KEYID:
        case CDK_DBSEARCH_KEYID:
          p = desc;
          if (!strncmp (p, "0x", 2))
            p += 2;
          if (strlen (p) == 8)
            {
              dbs->u.keyid[0] = 0;
              dbs->u.keyid[1] = strtoul (p, NULL, 16);
            }
          else if (strlen (p) == 16)
            {
              dbs->u.keyid[0] = strtoul (p, NULL, 16);
              dbs->u.keyid[1] = strtoul (p + 8, NULL, 16);
            }
          else
            {                   /* Invalid key ID object. */
              cdk_free (dbs);
              return CDK_Inv_Mode;
            }
          break;

        case CDK_DBSEARCH_FPR:
          p = desc;
          if (strlen (p) != 2 * KEY_FPR_LEN)
            {
              cdk_free (dbs);
              return CDK_Inv_Mode;
            }
          for (i = 0; i < KEY_FPR_LEN; i++)
            {
              tmp[0] = p[2 * i];
              tmp[1] = p[2 * i + 1];
              tmp[2] = 0x00;
              dbs->u.fpr[i] = strtoul (tmp, NULL, 16);
            }
          break;
        }
      break;

    default:
      cdk_free (dbs);
      _cdk_log_debug ("cdk_keydb_search_start: invalid mode = %d\n", type);
      return CDK_Inv_Mode;
    }

  keydb_search_free (db->dbs);
  db->dbs = dbs;
  return 0;
}


static cdk_error_t
keydb_pos_from_cache (cdk_keydb_hd_t hd, cdk_dbsearch_t ks,
                      int *r_cache_hit, off_t * r_off)
{
  key_table_t c;

  if (!hd || !r_cache_hit || !r_off)
    return CDK_Inv_Value;

  /* Reset the values. */
  *r_cache_hit = 0;
  *r_off = 0;

  c = keydb_cache_find (hd->cache, ks);
  if (c != NULL)
    {
      _cdk_log_debug ("cache: found entry in cache.\n");
      *r_cache_hit = 1;
      *r_off = c->offset;
      return 0;
    }

  /* No index cache available so we just return here. */
  if (!hd->idx)
    return 0;

  if (hd->idx)
    {
      if (ks->type == CDK_DBSEARCH_KEYID)
        {
          if (keydb_idx_search (hd->idx, ks->u.keyid, NULL, r_off))
            return CDK_Error_No_Key;
          _cdk_log_debug ("cache: found keyid entry in idx table.\n");
          *r_cache_hit = 1;
        }
      else if (ks->type == CDK_DBSEARCH_FPR)
        {
          if (keydb_idx_search (hd->idx, NULL, ks->u.fpr, r_off))
            return CDK_Error_No_Key;
          _cdk_log_debug ("cache: found fpr entry in idx table.\n");
          *r_cache_hit = 1;
        }
    }

  return 0;
}


/**
 * cdk_keydb_search:
 * @hd: the keydb object
 * @ks: the keydb search object
 * @ret_key: kbnode object to store the key
 *
 * Search for a key in the given keyring. The search mode is handled
 * via @ks. If the key was found, @ret_key contains the key data.
 **/
cdk_error_t
cdk_keydb_search (cdk_keydb_hd_t hd, cdk_kbnode_t * ret_key)
{
  cdk_stream_t kr;
  cdk_kbnode_t knode;
  cdk_dbsearch_t ks;
  cdk_error_t rc = 0;
  off_t pos = 0, off = 0;
  int key_found = 0, cache_hit = 0;

  if (!hd || !ret_key)
    return CDK_Inv_Value;

  *ret_key = NULL;
  kr = NULL;
  hd->search = 1;
  rc = _cdk_keydb_open (hd, &kr);
  if (rc)
    return rc;

  if (!hd->no_cache)
    {
      /* It is possible the index is not up-to-date and thus we do
         not find the requesed key. In this case, we reset cache hit
         and continue our normal search procedure. */
      rc = keydb_pos_from_cache (hd, hd->dbs, &cache_hit, &off);
      if (rc)
        cache_hit = 0;
    }

  knode = NULL;
  ks = hd->dbs;
  while (!key_found && !rc)
    {
      if (cache_hit && ks->type != CDK_DBSEARCH_NEXT)
        cdk_stream_seek (kr, off);
      pos = cdk_stream_tell (kr);
      rc = cdk_keydb_get_keyblock (kr, &knode);
      if (rc)
        {
          if (rc == CDK_EOF)
            break;
          else
            return rc;
        }

      switch (ks->type)
        {
        case CDK_DBSEARCH_SHORT_KEYID:
        case CDK_DBSEARCH_KEYID:
          key_found = find_by_keyid (knode, ks);
          break;

        case CDK_DBSEARCH_FPR:
          key_found = find_by_fpr (knode, ks);
          break;

        case CDK_DBSEARCH_EXACT:
        case CDK_DBSEARCH_SUBSTR:
          key_found = find_by_pattern (knode, ks);
          break;

        case CDK_DBSEARCH_NEXT:
          key_found = knode ? 1 : 0;
          break;
        }

      if (key_found)
        {
          if (!keydb_cache_find (hd->cache, ks))
            keydb_cache_add (hd, ks, pos);
          break;
        }

      cdk_kbnode_release (knode);
      knode = NULL;
    }

  hd->search = 0;
  if (key_found && rc == CDK_EOF)
    rc = 0;
  else if (rc == CDK_EOF && !key_found)
    rc = CDK_Error_No_Key;
  *ret_key = key_found ? knode : NULL;
  return rc;
}


cdk_error_t
cdk_keydb_get_bykeyid (cdk_keydb_hd_t hd, u32 * keyid, cdk_kbnode_t * ret_key)
{
  cdk_error_t rc;

  if (!hd || !keyid || !ret_key)
    return CDK_Inv_Value;

  rc = cdk_keydb_search_start (hd, CDK_DBSEARCH_KEYID, keyid);
  if (!rc)
    rc = cdk_keydb_search (hd, ret_key);
  return rc;
}


cdk_error_t
cdk_keydb_get_byfpr (cdk_keydb_hd_t hd, const byte * fpr,
                     cdk_kbnode_t * r_key)
{
  cdk_error_t rc;

  if (!hd || !fpr || !r_key)
    return CDK_Inv_Value;

  rc = cdk_keydb_search_start (hd, CDK_DBSEARCH_FPR, (byte *) fpr);
  if (!rc)
    rc = cdk_keydb_search (hd, r_key);
  return rc;
}


cdk_error_t
cdk_keydb_get_bypattern (cdk_keydb_hd_t hd, const char *patt,
                         cdk_kbnode_t * ret_key)
{
  cdk_error_t rc;

  if (!hd || !patt || !ret_key)
    return CDK_Inv_Value;

  rc = cdk_keydb_search_start (hd, CDK_DBSEARCH_SUBSTR, (char *) patt);
  if (!rc)
    rc = cdk_keydb_search (hd, ret_key);
  return rc;
}


static int
keydb_check_key (cdk_packet_t pkt)
{
  cdk_pkt_pubkey_t pk;
  int is_sk, valid;

  if (pkt->pkttype == CDK_PKT_PUBLIC_KEY ||
      pkt->pkttype == CDK_PKT_PUBLIC_SUBKEY)
    {
      pk = pkt->pkt.public_key;
      is_sk = 0;
    }
  else if (pkt->pkttype == CDK_PKT_SECRET_KEY ||
           pkt->pkttype == CDK_PKT_SECRET_SUBKEY)
    {
      pk = pkt->pkt.secret_key->pk;
      is_sk = 1;
    }
  else                          /* No key object. */
    return 0;
  valid = !pk->is_revoked && !pk->has_expired;
  if (is_sk)
    return valid;
  return valid && !pk->is_invalid;
}


/* Find the first kbnode with the requested packet type
   that represents a valid key. */
static cdk_kbnode_t
kbnode_find_valid (cdk_kbnode_t root, int pkttype)
{
  cdk_kbnode_t n;

  for (n = root; n; n = n->next)
    {
      if (n->pkt->pkttype != pkttype)
        continue;
      if (keydb_check_key (n->pkt))
        return n;
    }

  return NULL;
}


static cdk_kbnode_t
keydb_find_byusage (cdk_kbnode_t root, int req_usage, int is_pk)
{
  cdk_kbnode_t node, key;
  int req_type;
  long timestamp;

  req_type = is_pk ? CDK_PKT_PUBLIC_KEY : CDK_PKT_SECRET_KEY;
  if (!req_usage)
    return kbnode_find_valid (root, req_type);

  node = cdk_kbnode_find (root, req_type);
  if (node && !keydb_check_key (node->pkt))
    return NULL;

  key = NULL;
  timestamp = 0;
  /* We iteratre over the all nodes and search for keys or
     subkeys which match the usage and which are not invalid.
     A timestamp is used to figure out the newest valid key. */
  for (node = root; node; node = node->next)
    {
      if (is_pk && (node->pkt->pkttype == CDK_PKT_PUBLIC_KEY ||
                    node->pkt->pkttype == CDK_PKT_PUBLIC_SUBKEY)
          && keydb_check_key (node->pkt)
          && (node->pkt->pkt.public_key->pubkey_usage & req_usage))
        {
          if (node->pkt->pkt.public_key->timestamp > timestamp)
            key = node;
        }
      if (!is_pk && (node->pkt->pkttype == CDK_PKT_SECRET_KEY ||
                     node->pkt->pkttype == CDK_PKT_SECRET_SUBKEY)
          && keydb_check_key (node->pkt)
          && (node->pkt->pkt.secret_key->pk->pubkey_usage & req_usage))
        {
          if (node->pkt->pkt.secret_key->pk->timestamp > timestamp)
            key = node;
        }

    }
  return key;
}


static cdk_kbnode_t
keydb_find_bykeyid (cdk_kbnode_t root, const u32 * keyid, int search_mode)
{
  cdk_kbnode_t node;
  u32 kid[2];

  for (node = root; node; node = node->next)
    {
      if (!_cdk_pkt_get_keyid (node->pkt, kid))
        continue;
      if (search_mode == CDK_DBSEARCH_SHORT_KEYID && kid[1] == keyid[1])
        return node;
      else if (kid[0] == keyid[0] && kid[1] == keyid[1])
        return node;
    }
  return NULL;
}


cdk_error_t
_cdk_keydb_get_sk_byusage (cdk_keydb_hd_t hd, const char *name,
                           cdk_seckey_t * ret_sk, int usage)
{
  cdk_kbnode_t knode = NULL;
  cdk_kbnode_t node, sk_node, pk_node;
  cdk_pkt_seckey_t sk;
  cdk_error_t rc;
  const char *s;
  int pkttype;

  if (!ret_sk || !usage)
    return CDK_Inv_Value;
  if (!hd)
    return CDK_Error_No_Keyring;

  *ret_sk = NULL;
  rc = cdk_keydb_search_start (hd, CDK_DBSEARCH_AUTO, (char *) name);
  if (rc)
    return rc;

  rc = cdk_keydb_search (hd, &knode);
  if (rc)
    return rc;

  sk_node = keydb_find_byusage (knode, usage, 0);
  if (!sk_node)
    {
      cdk_kbnode_release (knode);
      return CDK_Unusable_Key;
    }

  /* We clone the node with the secret key to avoid that the
     packet will be released. */
  _cdk_kbnode_clone (sk_node);
  sk = sk_node->pkt->pkt.secret_key;

  for (node = knode; node; node = node->next)
    {
      if (node->pkt->pkttype == CDK_PKT_USER_ID)
        {
          s = node->pkt->pkt.user_id->name;
          if (sk && !sk->pk->uid && _cdk_memistr (s, strlen (s), name))
            {
              _cdk_copy_userid (&sk->pk->uid, node->pkt->pkt.user_id);
              break;
            }
        }
    }

  /* To find the self signature, we need the primary public key because
     the selected secret key might be different from the primary key. */
  pk_node = cdk_kbnode_find (knode, CDK_PKT_SECRET_KEY);
  if (!pk_node)
    {
      cdk_kbnode_release (knode);
      return CDK_Unusable_Key;
    }
  node = find_selfsig_node (knode, pk_node->pkt->pkt.secret_key->pk);
  if (sk->pk->uid && node)
    _cdk_copy_signature (&sk->pk->uid->selfsig, node->pkt->pkt.signature);

  /* We only release the outer packet. */
  _cdk_pkt_detach_free (sk_node->pkt, &pkttype, (void *) &sk);
  cdk_kbnode_release (knode);
  *ret_sk = sk;
  return rc;
}


cdk_error_t
_cdk_keydb_get_pk_byusage (cdk_keydb_hd_t hd, const char *name,
                           cdk_pubkey_t * ret_pk, int usage)
{
  cdk_kbnode_t knode, node, pk_node;
  cdk_pkt_pubkey_t pk;
  const char *s;
  cdk_error_t rc;

  if (!ret_pk || !usage)
    return CDK_Inv_Value;
  if (!hd)
    return CDK_Error_No_Keyring;

  *ret_pk = NULL;
  rc = cdk_keydb_search_start (hd, CDK_DBSEARCH_AUTO, (char *) name);
  if (!rc)
    rc = cdk_keydb_search (hd, &knode);
  if (rc)
    return rc;

  node = keydb_find_byusage (knode, usage, 1);
  if (!node)
    {
      cdk_kbnode_release (knode);
      return CDK_Unusable_Key;
    }

  pk = NULL;
  _cdk_copy_pubkey (&pk, node->pkt->pkt.public_key);
  for (node = knode; node; node = node->next)
    {
      if (node->pkt->pkttype == CDK_PKT_USER_ID)
        {
          s = node->pkt->pkt.user_id->name;
          if (pk && !pk->uid && _cdk_memistr (s, strlen (s), name))
            {
              _cdk_copy_userid (&pk->uid, node->pkt->pkt.user_id);
              break;
            }
        }
    }

  /* Same as in the sk code, the selected key can be a sub key 
     and thus we need the primary key to find the self sig. */
  pk_node = cdk_kbnode_find (knode, CDK_PKT_PUBLIC_KEY);
  if (!pk_node)
    {
      cdk_kbnode_release (knode);
      return CDK_Unusable_Key;
    }
  node = find_selfsig_node (knode, pk_node->pkt->pkt.public_key);
  if (pk->uid && node)
    _cdk_copy_signature (&pk->uid->selfsig, node->pkt->pkt.signature);
  cdk_kbnode_release (knode);

  *ret_pk = pk;
  return rc;
}


/**
 * cdk_keydb_get_pk:
 * @hd: key db handle
 * @keyid: keyid of the key
 * @r_pk: the allocated public key
 * 
 * Perform a key database search by keyid and return the raw public
 * key without any signatures or user id's.
 **/
cdk_error_t
cdk_keydb_get_pk (cdk_keydb_hd_t hd, u32 * keyid, cdk_pubkey_t * r_pk)
{
  cdk_kbnode_t knode = NULL, node;
  cdk_pubkey_t pk;
  cdk_error_t rc;
  size_t s_type;
  int pkttype;

  if (!keyid || !r_pk)
    return CDK_Inv_Value;
  if (!hd)
    return CDK_Error_No_Keyring;

  *r_pk = NULL;
  s_type = !keyid[0] ? CDK_DBSEARCH_SHORT_KEYID : CDK_DBSEARCH_KEYID;
  rc = cdk_keydb_search_start (hd, s_type, keyid);
  if (rc)
    return rc;
  rc = cdk_keydb_search (hd, &knode);
  if (rc)
    return rc;

  node = keydb_find_bykeyid (knode, keyid, s_type);
  if (!node)
    {
      cdk_kbnode_release (knode);
      return CDK_Error_No_Key;
    }

  /* See comment in cdk_keydb_get_sk() */
  _cdk_pkt_detach_free (node->pkt, &pkttype, (void *) &pk);
  *r_pk = pk;
  _cdk_kbnode_clone (node);
  cdk_kbnode_release (knode);

  return rc;
}


/**
 * cdk_keydb_get_sk:
 * @hd: key db handle
 * @keyid: the keyid of the key
 * @ret_sk: the allocated secret key
 * 
 * Perform a key database search by keyid and return
 * only the raw secret key without the additional nodes,
 * like the user id or the signatures.
 **/
cdk_error_t
cdk_keydb_get_sk (cdk_keydb_hd_t hd, u32 * keyid, cdk_seckey_t * ret_sk)
{
  cdk_kbnode_t snode, node;
  cdk_seckey_t sk;
  cdk_error_t rc;
  int pkttype;

  if (!keyid || !ret_sk)
    return CDK_Inv_Value;
  if (!hd)
    return CDK_Error_No_Keyring;

  *ret_sk = NULL;
  rc = cdk_keydb_get_bykeyid (hd, keyid, &snode);
  if (rc)
    return rc;

  node = keydb_find_bykeyid (snode, keyid, CDK_DBSEARCH_KEYID);
  if (!node)
    {
      cdk_kbnode_release (snode);
      return CDK_Error_No_Key;
    }

  /* We need to release the packet itself but not its contents
     and thus we detach the openpgp packet and release the structure. */
  _cdk_pkt_detach_free (node->pkt, &pkttype, (void *) &sk);
  _cdk_kbnode_clone (node);
  cdk_kbnode_release (snode);

  *ret_sk = sk;
  return 0;
}


static int
is_selfsig (cdk_kbnode_t node, const u32 * keyid)
{
  cdk_pkt_signature_t sig;

  if (node->pkt->pkttype != CDK_PKT_SIGNATURE)
    return 0;
  sig = node->pkt->pkt.signature;
  if ((sig->sig_class >= 0x10 && sig->sig_class <= 0x13) &&
      sig->keyid[0] == keyid[0] && sig->keyid[1] == keyid[1])
    return 1;

  return 0;
}


/* Find the newest self signature for the public key @pk
   and return the signature node. */
static cdk_kbnode_t
find_selfsig_node (cdk_kbnode_t key, cdk_pkt_pubkey_t pk)
{
  cdk_kbnode_t n, sig;
  unsigned int ts;
  u32 keyid[2];

  cdk_pk_get_keyid (pk, keyid);
  sig = NULL;
  ts = 0;
  for (n = key; n; n = n->next)
    {
      if (is_selfsig (n, keyid) && n->pkt->pkt.signature->timestamp > ts)
        {
          ts = n->pkt->pkt.signature->timestamp;
          sig = n;
        }
    }

  return sig;
}



static cdk_error_t
keydb_merge_selfsig (cdk_kbnode_t key, u32 * keyid)
{
  cdk_kbnode_t node, kbnode, unode;
  cdk_subpkt_t s = NULL;
  cdk_pkt_signature_t sig = NULL;
  cdk_pkt_userid_t uid = NULL;
  const byte *symalg = NULL, *hashalg = NULL, *compalg = NULL;
  size_t nsymalg = 0, nhashalg = 0, ncompalg = 0, n = 0;
  size_t key_usage = 0, key_expire = 0;

  if (!key)
    return CDK_Inv_Value;

  for (node = key; node; node = node->next)
    {
      if (!is_selfsig (node, keyid))
        continue;
      unode = cdk_kbnode_find_prev (key, node, CDK_PKT_USER_ID);
      if (!unode)
        return CDK_Error_No_Key;
      uid = unode->pkt->pkt.user_id;
      sig = node->pkt->pkt.signature;
      s = cdk_subpkt_find (sig->hashed, CDK_SIGSUBPKT_PRIMARY_UID);
      if (s)
        uid->is_primary = 1;
      s = cdk_subpkt_find (sig->hashed, CDK_SIGSUBPKT_FEATURES);
      if (s && s->size == 1 && s->d[0] & 0x01)
        uid->mdc_feature = 1;
      s = cdk_subpkt_find (sig->hashed, CDK_SIGSUBPKT_KEY_EXPIRE);
      if (s && s->size == 4)
        key_expire = _cdk_buftou32 (s->d);
      s = cdk_subpkt_find (sig->hashed, CDK_SIGSUBPKT_KEY_FLAGS);
      if (s)
        {
          if (s->d[0] & 0x03)   /* cert + sign data */
            key_usage |= CDK_KEY_USG_SIGN;
          if (s->d[0] & 0x0C)   /* encrypt comm. + storage */
            key_usage |= CDK_KEY_USG_ENCR;
          if (s->d[0] & 0x20)
            key_usage |= CDK_KEY_USG_AUTH;
        }
      s = cdk_subpkt_find (sig->hashed, CDK_SIGSUBPKT_PREFS_SYM);
      if (s)
        {
          symalg = s->d;
          nsymalg = s->size;
          n += s->size + 1;
        }
      s = cdk_subpkt_find (sig->hashed, CDK_SIGSUBPKT_PREFS_HASH);
      if (s)
        {
          hashalg = s->d;
          nhashalg = s->size;
          n += s->size + 1;
        }
      s = cdk_subpkt_find (sig->hashed, CDK_SIGSUBPKT_PREFS_ZIP);
      if (s)
        {
          compalg = s->d;
          ncompalg = s->size;
          n += s->size + 1;
        }
      if (uid->prefs != NULL)
        cdk_free (uid->prefs);
      if (!n || !hashalg || !compalg || !symalg)
        uid->prefs = NULL;
      else
        {
          uid->prefs = cdk_calloc (1, sizeof (*uid->prefs) * (n + 1));
          if (!uid->prefs)
            return CDK_Out_Of_Core;
          n = 0;
          for (; nsymalg; nsymalg--, n++)
            {
              uid->prefs[n].type = CDK_PREFTYPE_SYM;
              uid->prefs[n].value = *symalg++;
            }
          for (; nhashalg; nhashalg--, n++)
            {
              uid->prefs[n].type = CDK_PREFTYPE_HASH;
              uid->prefs[n].value = *hashalg++;
            }
          for (; ncompalg; ncompalg--, n++)
            {
              uid->prefs[n].type = CDK_PREFTYPE_ZIP;
              uid->prefs[n].value = *compalg++;
            }

          uid->prefs[n].type = CDK_PREFTYPE_NONE;       /* end of list marker */
          uid->prefs[n].value = 0;
          uid->prefs_size = n;
        }
    }

  /* Now we add the extracted information to the primary key. */
  kbnode = cdk_kbnode_find (key, CDK_PKT_PUBLIC_KEY);
  if (kbnode)
    {
      cdk_pkt_pubkey_t pk = kbnode->pkt->pkt.public_key;
      if (uid && uid->prefs && n)
        {
          if (pk->prefs != NULL)
            cdk_free (pk->prefs);
          pk->prefs = _cdk_copy_prefs (uid->prefs);
          pk->prefs_size = n;
        }
      if (key_expire)
        {
          pk->expiredate = pk->timestamp + key_expire;
          pk->has_expired = pk->expiredate > (u32) time (NULL) ? 0 : 1;
        }

      if (key_usage)
        pk->pubkey_usage = key_usage;
      pk->is_invalid = 0;
    }

  return 0;
}


static cdk_error_t
keydb_parse_allsigs (cdk_kbnode_t knode, cdk_keydb_hd_t hd, int check)
{
  cdk_kbnode_t node, kb;
  cdk_pkt_signature_t sig;
  cdk_pkt_pubkey_t pk;
  cdk_subpkt_t s = NULL;
  u32 expiredate = 0, curtime = (u32) time (NULL);
  u32 keyid[2];

  if (!knode)
    return CDK_Inv_Value;
  if (check && !hd)
    return CDK_Inv_Mode;

  kb = cdk_kbnode_find (knode, CDK_PKT_SECRET_KEY);
  if (kb)
    return 0;

  /* Reset */
  for (node = knode; node; node = node->next)
    {
      if (node->pkt->pkttype == CDK_PKT_USER_ID)
        node->pkt->pkt.user_id->is_revoked = 0;
      else if (node->pkt->pkttype == CDK_PKT_PUBLIC_KEY ||
               node->pkt->pkttype == CDK_PKT_PUBLIC_SUBKEY)
        node->pkt->pkt.public_key->is_revoked = 0;
    }

  kb = cdk_kbnode_find (knode, CDK_PKT_PUBLIC_KEY);
  if (!kb)
    return CDK_Wrong_Format;
  cdk_pk_get_keyid (kb->pkt->pkt.public_key, keyid);

  for (node = knode; node; node = node->next)
    {
      if (node->pkt->pkttype == CDK_PKT_SIGNATURE)
        {
          sig = node->pkt->pkt.signature;
          /* Revocation certificates for primary keys */
          if (sig->sig_class == 0x20)
            {
              kb = cdk_kbnode_find_prev (knode, node, CDK_PKT_PUBLIC_KEY);
              if (kb)
                {
                  kb->pkt->pkt.public_key->is_revoked = 1;
                  if (check)
                    _cdk_pk_check_sig (hd, kb, node, NULL);
                }
              else
                return CDK_Error_No_Key;
            }
          /* Revocation certificates for subkeys */
          else if (sig->sig_class == 0x28)
            {
              kb = cdk_kbnode_find_prev (knode, node, CDK_PKT_PUBLIC_SUBKEY);
              if (kb)
                {
                  kb->pkt->pkt.public_key->is_revoked = 1;
                  if (check)
                    _cdk_pk_check_sig (hd, kb, node, NULL);
                }
              else
                return CDK_Error_No_Key;
            }
          /* Revocation certifcates for user ID's */
          else if (sig->sig_class == 0x30)
            {
              if (sig->keyid[0] != keyid[0] || sig->keyid[1] != keyid[1])
                continue;       /* revokes an earlier signature, no userID. */
              kb = cdk_kbnode_find_prev (knode, node, CDK_PKT_USER_ID);
              if (kb)
                {
                  kb->pkt->pkt.user_id->is_revoked = 1;
                  if (check)
                    _cdk_pk_check_sig (hd, kb, node, NULL);
                }
              else
                return CDK_Error_No_Key;
            }
          /* Direct certificates for primary keys */
          else if (sig->sig_class == 0x1F)
            {
              kb = cdk_kbnode_find_prev (knode, node, CDK_PKT_PUBLIC_KEY);
              if (kb)
                {
                  pk = kb->pkt->pkt.public_key;
                  pk->is_invalid = 0;
                  s = cdk_subpkt_find (node->pkt->pkt.signature->hashed,
                                       CDK_SIGSUBPKT_KEY_EXPIRE);
                  if (s)
                    {
                      expiredate = _cdk_buftou32 (s->d);
                      pk->expiredate = pk->timestamp + expiredate;
                      pk->has_expired = pk->expiredate > curtime ? 0 : 1;
                    }
                  if (check)
                    _cdk_pk_check_sig (hd, kb, node, NULL);
                }
              else
                return CDK_Error_No_Key;
            }
          /* Direct certificates for subkeys */
          else if (sig->sig_class == 0x18)
            {
              kb = cdk_kbnode_find_prev (knode, node, CDK_PKT_PUBLIC_SUBKEY);
              if (kb)
                {
                  pk = kb->pkt->pkt.public_key;
                  pk->is_invalid = 0;
                  s = cdk_subpkt_find (node->pkt->pkt.signature->hashed,
                                       CDK_SIGSUBPKT_KEY_EXPIRE);
                  if (s)
                    {
                      expiredate = _cdk_buftou32 (s->d);
                      pk->expiredate = pk->timestamp + expiredate;
                      pk->has_expired = pk->expiredate > curtime ? 0 : 1;
                    }
                  if (check)
                    _cdk_pk_check_sig (hd, kb, node, NULL);
                }
              else
                return CDK_Error_No_Key;
            }
        }
    }
  node = cdk_kbnode_find (knode, CDK_PKT_PUBLIC_KEY);
  if (node && node->pkt->pkt.public_key->version == 3)
    {
      /* v3 public keys have no additonal signatures for the key directly.
         we say the key is valid when we have at least a self signature. */
      pk = node->pkt->pkt.public_key;
      for (node = knode; node; node = node->next)
        {
          if (is_selfsig (node, keyid))
            {
              pk->is_invalid = 0;
              break;
            }
        }
    }
  if (node && (node->pkt->pkt.public_key->is_revoked ||
               node->pkt->pkt.public_key->has_expired))
    {
      /* If the primary key has been revoked, mark all subkeys as invalid
         because without a primary key they are not useable */
      for (node = knode; node; node = node->next)
        {
          if (node->pkt->pkttype == CDK_PKT_PUBLIC_SUBKEY)
            node->pkt->pkt.public_key->is_invalid = 1;
        }
    }

  return 0;
}


cdk_error_t
cdk_keydb_get_keyblock (cdk_stream_t inp, cdk_kbnode_t * r_knode)
{
  cdk_packet_t pkt;
  cdk_kbnode_t knode, node;
  cdk_desig_revoker_t revkeys;
  cdk_error_t rc;
  u32 keyid[2], main_keyid[2];
  off_t old_off;
  int key_seen, got_key;

  if (!inp || !r_knode)
    return CDK_Inv_Value;

  /* Reset all values. */
  keyid[0] = keyid[1] = 0;
  main_keyid[0] = main_keyid[1] = 0;
  revkeys = NULL;
  knode = NULL;
  key_seen = got_key = 0;

  *r_knode = NULL;
  rc = CDK_EOF;
  while (!cdk_stream_eof (inp))
    {
      cdk_pkt_new (&pkt);
      old_off = cdk_stream_tell (inp);
      rc = cdk_pkt_read (inp, pkt);
      if (rc)
        {
          cdk_pkt_release (pkt);
          if (rc == CDK_EOF)
            break;
          else
            {                   /* Release all packets we reached so far. */
              _cdk_log_debug ("keydb_get_keyblock: error %d\n", rc);
              cdk_kbnode_release (knode);
              return rc;
            }
        }
      if (pkt->pkttype == CDK_PKT_PUBLIC_KEY ||
          pkt->pkttype == CDK_PKT_PUBLIC_SUBKEY ||
          pkt->pkttype == CDK_PKT_SECRET_KEY ||
          pkt->pkttype == CDK_PKT_SECRET_SUBKEY)
        {
          if (key_seen && (pkt->pkttype == CDK_PKT_PUBLIC_KEY ||
                           pkt->pkttype == CDK_PKT_SECRET_KEY))
            {
              /* The next key starts here so set the file pointer
                 and leave the loop. */
              cdk_stream_seek (inp, old_off);
              cdk_pkt_release (pkt);
              break;
            }
          if (pkt->pkttype == CDK_PKT_PUBLIC_KEY ||
              pkt->pkttype == CDK_PKT_SECRET_KEY)
            {
              _cdk_pkt_get_keyid (pkt, main_keyid);
              key_seen = 1;
            }
          else if (pkt->pkttype == CDK_PKT_PUBLIC_SUBKEY ||
                   pkt->pkttype == CDK_PKT_SECRET_SUBKEY)
            {
              if (pkt->pkttype == CDK_PKT_PUBLIC_SUBKEY)
                {
                  pkt->pkt.public_key->main_keyid[0] = main_keyid[0];
                  pkt->pkt.public_key->main_keyid[1] = main_keyid[1];
                }
              else
                {
                  pkt->pkt.secret_key->main_keyid[0] = main_keyid[0];
                  pkt->pkt.secret_key->main_keyid[1] = main_keyid[1];
                }
            }
          /* We save this for the signature */
          _cdk_pkt_get_keyid (pkt, keyid);
          got_key = 1;
        }
      else if (pkt->pkttype == CDK_PKT_USER_ID)
        ;
      else if (pkt->pkttype == CDK_PKT_SIGNATURE)
        {
          pkt->pkt.signature->key[0] = keyid[0];
          pkt->pkt.signature->key[1] = keyid[1];
          if (pkt->pkt.signature->sig_class == 0x1F &&
              pkt->pkt.signature->revkeys)
            revkeys = pkt->pkt.signature->revkeys;
        }
      node = cdk_kbnode_new (pkt);
      if (!knode)
        knode = node;
      else
        _cdk_kbnode_add (knode, node);
    }

  if (got_key)
    {
      keydb_merge_selfsig (knode, main_keyid);
      rc = keydb_parse_allsigs (knode, NULL, 0);
      if (revkeys)
        {
          node = cdk_kbnode_find (knode, CDK_PKT_PUBLIC_KEY);
          if (node)
            node->pkt->pkt.public_key->revkeys = revkeys;
        }
    }
  else
    cdk_kbnode_release (knode);
  *r_knode = got_key ? knode : NULL;

  /* It is possible that we are in an EOF condition after we
     successfully read a keyblock. For example if the requested
     key is the last in the file. */
  if (rc == CDK_EOF && got_key)
    rc = 0;
  return rc;
}


/* Return the type of the given data. In case it cannot be classified,
   a substring search will be performed. */
static int
classify_data (const byte * buf, size_t len)
{
  int type;
  int i;

  if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X'))
    {                           /* Skip hex prefix. */
      buf += 2;
      len -= 2;
    }

  /* The length of the data does not match either a keyid or a fingerprint. */
  if (len != 8 && len != 16 && len != 40)
    return CDK_DBSEARCH_SUBSTR;

  for (i = 0; i < len; i++)
    {
      if (!isxdigit (buf[i]))
        return CDK_DBSEARCH_SUBSTR;
    }
  if (i != len)
    return CDK_DBSEARCH_SUBSTR;
  switch (len)
    {
    case 8:
      type = CDK_DBSEARCH_SHORT_KEYID;
      break;
    case 16:
      type = CDK_DBSEARCH_KEYID;
      break;
    case 40:
      type = CDK_DBSEARCH_FPR;
      break;
    default:
      type = CDK_DBSEARCH_SUBSTR;
      break;
    }

  return type;
}


/**
 * cdk_keydb_export:
 * @hd: the keydb handle
 * @out: the output stream
 * @remusr: the list of key pattern to export
 *
 * Export a list of keys to the given output stream.
 * Use string list with names for pattering searching.
 * This procedure strips local signatures.
 **/
cdk_error_t
cdk_keydb_export (cdk_keydb_hd_t hd, cdk_stream_t out, cdk_strlist_t remusr)
{
  cdk_kbnode_t knode, node;
  cdk_strlist_t r;
  cdk_error_t rc;
  int old_ctb;

  for (r = remusr; r; r = r->next)
    {
      rc = cdk_keydb_search_start (hd, CDK_DBSEARCH_AUTO, r->d);
      if (rc)
        return rc;
      rc = cdk_keydb_search (hd, &knode);
      if (rc)
        return rc;
      node = cdk_kbnode_find (knode, CDK_PKT_PUBLIC_KEY);
      if (!node)
        return CDK_Error_No_Key;

      /* If the key is a version 3 key, use the old packet
         format for the output. */
      if (node->pkt->pkt.public_key->version == 3)
        old_ctb = 1;
      else
        old_ctb = 0;

      for (node = knode; node; node = node->next)
        {
          /* No specified format; skip them */
          if (node->pkt->pkttype == CDK_PKT_RING_TRUST)
            continue;
          /* We never export local signed signatures */
          if (node->pkt->pkttype == CDK_PKT_SIGNATURE &&
              !node->pkt->pkt.signature->flags.exportable)
            continue;
          /* Filter out invalid signatures */
          if (node->pkt->pkttype == CDK_PKT_SIGNATURE &&
              !KEY_CAN_SIGN (node->pkt->pkt.signature->pubkey_algo))
            continue;

          /* Adjust the ctb flag if needed. */
          node->pkt->old_ctb = old_ctb;
          rc = cdk_pkt_write (out, node->pkt);
          if (rc)
            {
              cdk_kbnode_release (knode);
              return rc;
            }
        }
      cdk_kbnode_release (knode);
      knode = NULL;
    }
  return 0;
}


static cdk_packet_t
find_key_packet (cdk_kbnode_t knode, int *r_is_sk)
{
  cdk_packet_t pkt;

  pkt = cdk_kbnode_find_packet (knode, CDK_PKT_PUBLIC_KEY);
  if (!pkt)
    {
      pkt = cdk_kbnode_find_packet (knode, CDK_PKT_SECRET_KEY);
      if (r_is_sk)
        *r_is_sk = pkt ? 1 : 0;
    }
  return pkt;
}


/* Return 1 if the is allowd in a key node. */
static int
is_key_node (cdk_kbnode_t node)
{
  switch (node->pkt->pkttype)
    {
    case CDK_PKT_SIGNATURE:
    case CDK_PKT_SECRET_KEY:
    case CDK_PKT_PUBLIC_KEY:
    case CDK_PKT_SECRET_SUBKEY:
    case CDK_PKT_PUBLIC_SUBKEY:
    case CDK_PKT_USER_ID:
    case CDK_PKT_ATTRIBUTE:
      return 1;

    default:
      return 0;
    }

  return 0;
}


cdk_error_t
cdk_keydb_import (cdk_keydb_hd_t hd, cdk_kbnode_t knode)
{
  cdk_kbnode_t node, chk;
  cdk_packet_t pkt;
  cdk_stream_t out;
  cdk_error_t rc;
  u32 keyid[2];

  if (!hd || !knode)
    return CDK_Inv_Value;

  pkt = find_key_packet (knode, NULL);
  if (!pkt)
    return CDK_Inv_Packet;

  _cdk_pkt_get_keyid (pkt, keyid);
  chk = NULL;
  cdk_keydb_get_bykeyid (hd, keyid, &chk);
  if (chk)
    {                           /* FIXME: search for new signatures */
      cdk_kbnode_release (chk);
      return 0;
    }

  /* We append data to the stream so we need to close
     the stream here to re-open it later. */
  if (hd->fp)
    {
      cdk_stream_close (hd->fp);
      hd->fp = NULL;
    }

  rc = _cdk_stream_append (hd->name, &out);
  if (rc)
    return rc;

  for (node = knode; node; node = node->next)
    {
      if (node->pkt->pkttype == CDK_PKT_RING_TRUST)
        continue;               /* No uniformed syntax for this packet */
      if (node->pkt->pkttype == CDK_PKT_SIGNATURE &&
          !node->pkt->pkt.signature->flags.exportable)
        {
          _cdk_log_debug ("key db import: skip local signature\n");
          continue;
        }

      if (!is_key_node (node))
        {
          _cdk_log_debug ("key db import: skip invalid node of type %d\n",
                          node->pkt->pkttype);
          continue;
        }

      rc = cdk_pkt_write (out, node->pkt);
      if (rc)
        {
          cdk_stream_close (out);
          return rc;
        }
    }

  cdk_stream_close (out);
  if (!hd->no_cache)
    cdk_keydb_idx_rebuild (hd);
  hd->stats.new_keys++;

  return 0;
}


cdk_error_t
_cdk_keydb_check_userid (cdk_keydb_hd_t hd, u32 * keyid, const char *id)
{
  cdk_kbnode_t knode = NULL, unode = NULL;
  cdk_error_t rc;
  int check;

  if (!hd)
    return CDK_Inv_Value;

  rc = cdk_keydb_search_start (hd, CDK_DBSEARCH_KEYID, keyid);
  if (rc)
    return rc;
  rc = cdk_keydb_search (hd, &knode);
  if (rc)
    return rc;

  rc = cdk_keydb_search_start (hd, CDK_DBSEARCH_EXACT, (char *) id);
  if (!rc)
    rc = cdk_keydb_search (hd, &unode);
  if (rc)
    {
      cdk_kbnode_release (knode);
      return rc;
    }

  check = 0;
  cdk_keydb_search_start (hd, CDK_DBSEARCH_KEYID, keyid);
  if (unode && find_by_keyid (unode, hd->dbs))
    check++;
  cdk_kbnode_release (unode);

  cdk_keydb_search_start (hd, CDK_DBSEARCH_EXACT, (char *) id);
  if (knode && find_by_pattern (knode, hd->dbs))
    check++;
  cdk_kbnode_release (knode);

  return check == 2 ? 0 : CDK_Inv_Value;
}


/**
 * cdk_keydb_check_sk:
 * @hd: the key db handle
 * @keyid: the 64-bit keyid
 * 
 * Check if a secret key with the given key ID is available
 * in the key database.
 **/
cdk_error_t
cdk_keydb_check_sk (cdk_keydb_hd_t hd, u32 * keyid)
{
  cdk_stream_t db;
  cdk_packet_t pkt;
  cdk_error_t rc;
  u32 kid[2];

  if (!hd || !keyid)
    return CDK_Inv_Value;
  if (!hd->secret)
    return CDK_Inv_Mode;

  rc = _cdk_keydb_open (hd, &db);
  if (rc)
    return rc;
  cdk_pkt_new (&pkt);
  while (!cdk_pkt_read (db, pkt))
    {
      if (pkt->pkttype != CDK_PKT_SECRET_KEY &&
          pkt->pkttype != CDK_PKT_SECRET_SUBKEY)
        {
          cdk_pkt_free (pkt);
          continue;
        }
      cdk_sk_get_keyid (pkt->pkt.secret_key, kid);
      if (KEYID_CMP (kid, keyid))
        {
          cdk_pkt_release (pkt);
          return 0;
        }
      cdk_pkt_free (pkt);
    }
  cdk_pkt_release (pkt);
  return CDK_Error_No_Key;
}


/**
 * cdk_listkey_start:
 * @r_ctx: pointer to store the new context
 * @db: the key database handle
 * @patt: string pattern
 * @fpatt: recipients from a stringlist to show
 *
 * Prepare a key listing with the given parameters. Two modes are supported.
 * The first mode uses string pattern to determine if the key should be
 * returned or not. The other mode uses a string list to request the key
 * which should be listed.
 **/
cdk_error_t
cdk_listkey_start (cdk_listkey_t * r_ctx, cdk_keydb_hd_t db,
                   const char *patt, cdk_strlist_t fpatt)
{
  cdk_listkey_t ctx;
  cdk_stream_t inp;
  cdk_error_t rc;

  if (!r_ctx || !db)
    return CDK_Inv_Value;
  if ((patt && fpatt) || (!patt && !fpatt))
    return CDK_Inv_Mode;
  rc = _cdk_keydb_open (db, &inp);
  if (rc)
    return rc;
  ctx = cdk_calloc (1, sizeof *ctx);
  if (!ctx)
    return CDK_Out_Of_Core;
  ctx->db = db;
  ctx->inp = inp;
  if (patt)
    {
      ctx->u.patt = cdk_strdup (patt);
      if (!ctx->u.patt)
        return CDK_Out_Of_Core;
    }
  else if (fpatt)
    {
      cdk_strlist_t l;
      for (l = fpatt; l; l = l->next)
        cdk_strlist_add (&ctx->u.fpatt, l->d);
    }
  ctx->type = patt ? 1 : 0;
  ctx->init = 1;
  *r_ctx = ctx;
  return 0;
}


/**
 * cdk_listkey_close:
 * @ctx: the list key context
 *
 * Free the list key context.
 **/
void
cdk_listkey_close (cdk_listkey_t ctx)
{
  if (!ctx)
    return;

  if (ctx->type)
    cdk_free (ctx->u.patt);
  else
    cdk_strlist_free (ctx->u.fpatt);
  cdk_free (ctx);
}


/**
 * cdk_listkey_next:
 * @ctx: list key context
 * @r_key: the pointer to the new key node object
 *
 * Retrieve the next key from the pattern of the key list context.
 **/
cdk_error_t
cdk_listkey_next (cdk_listkey_t ctx, cdk_kbnode_t * ret_key)
{
  if (!ctx || !ret_key)
    return CDK_Inv_Value;
  if (!ctx->init)
    return CDK_Inv_Mode;

  if (ctx->type && ctx->u.patt[0] == '*')
    return cdk_keydb_get_keyblock (ctx->inp, ret_key);
  else if (ctx->type)
    {
      cdk_kbnode_t node;
      struct cdk_dbsearch_s ks;
      cdk_error_t rc;

      for (;;)
        {
          rc = cdk_keydb_get_keyblock (ctx->inp, &node);
          if (rc)
            return rc;
          memset (&ks, 0, sizeof (ks));
          ks.type = CDK_DBSEARCH_SUBSTR;
          ks.u.pattern = ctx->u.patt;
          if (find_by_pattern (node, &ks))
            {
              *ret_key = node;
              return 0;
            }
          cdk_kbnode_release (node);
          node = NULL;
        }
    }
  else
    {
      if (!ctx->t)
        ctx->t = ctx->u.fpatt;
      else if (ctx->t->next)
        ctx->t = ctx->t->next;
      else
        return CDK_EOF;
      return cdk_keydb_get_bypattern (ctx->db, ctx->t->d, ret_key);
    }
  return CDK_General_Error;
}


int
_cdk_keydb_is_secret (cdk_keydb_hd_t db)
{
  return db->secret;
}
