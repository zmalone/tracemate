/**
 * Copyright 2020 Major League Baseball
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "tm_utils.h"
#include "tm_metric.h"

#include <curl/curl.h>

#include <mtev_b64.h>
#include <mtev_dyn_buffer.h>
#include <mtev_log.h>
#include <mtev_conf.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

static char *current_access_token = NULL;
static uint64_t last_token_create = 0;

/* looks at `str` and determines if it's a garbage path.  Things like:
 *
 * /../../../../../../etc/passwd
 *
 * and other obvious hack attempts
 */
bool is_path_ok(const char *str, team_data_t *td)
{
  /* we must URL decode in case there are %NN code in the string */
  int out_len = 0;
  CURL *curl = curl_easy_init();
  char *decoded = curl_easy_unescape(curl, str, 0, &out_len);
  curl_easy_cleanup(curl);

  if (decoded == NULL) {
    return false;
  }

  if (strstr(decoded, "..") != NULL ) {
    curl_free(decoded);
    return false;
  }

  if (strstr(decoded, "//") != NULL ) {
    curl_free(decoded);
    return false;
  }

  if (strstr(decoded, ".git") != NULL ) {
    curl_free(decoded);
    return false;
  }

  if (strstr(decoded, ".ssh") != NULL ) {
    curl_free(decoded);
    return false;
  }

  if (strstr(decoded, ".svn") != NULL ) {
    curl_free(decoded);
    return false;
  }

  if (strstr(decoded, ".vscode") != NULL ) {
    curl_free(decoded);
    return false;
  }

  if (strstr(decoded, ".htaccess") != NULL ) {
    curl_free(decoded);
    return false;
  }

  if (strstr(decoded, "etc") != NULL ) {
    curl_free(decoded);
    return false;
  }

  if (strstr(decoded, "php") != NULL ) {
    curl_free(decoded);
    return false;
  }

  if (strstr(decoded, "<script>") != NULL ) {
    curl_free(decoded);
    return false;
  }


  for (size_t i = 0; i < td->allowlist_count; i++) {
    if (strncmp(decoded, td->allowlist[i], strlen(td->allowlist[i])) == 0) {
      curl_free(decoded);
      return true;
    }
  }

  mtevL(mtev_debug, "Path denied: %s\n", decoded);
  curl_free(decoded);
  return false;
}

char *genericize_path(const char *str, team_data_t *td)
{
  char copy[4096];
  char temp[4096];
  int ovector[30];

  if (str == NULL) return strdup("/");

  int len = 0;
  CURL *curl = curl_easy_init();
  char *decoded = curl_easy_unescape(curl, str, 0, &len);
  curl_easy_cleanup(curl);

  strncpy(copy, decoded, len);
  copy[len] = '\0';
  curl_free(decoded);

  /*
   * for each regex we replace any matches with the replacement string and then send this newly formed string
   * through the rest of the regex list.  so:
   *
   * /api/v1/game/566017/a9015abd-b384-4ddb-a41d-9b1ef6119486/analytics/parsed
   *
   * When it passes through a regex like: ".*(\/\\d+?:,\\d+\/).*" with a replacement: "/{id}/" would come out:
   *
   * /api/v1/game/{id}/a9015abd-b384-4ddb-a41d-9b1ef6119486/analytics/parsed
   *
   * Then if another regex in the list was: ".*(\/[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12}\/).*", replacement: "/{guid}/"
   *
   * You would get:
   *
   * /api/v1/game/{id}/{guid}/analytics/parsed
   *
   * Note that it's possible there is more than 1 match in a path but each regex will only replace 1 thing at most:
   *
   * /api/v1/foo/566017/23435/123213/bar
   *
   * This lets us build specific regexes to catch non-generic path cases like:
   *
   * /api/v1.1/game/2012_10_18_sfnmlb_slnmlb_1/feed/live
   *
   * with a regex like: "^\/api/v1\.1\/game(\/[0-9]{4}_[0-9]{2}_[0-9]{2}_.*\/)feed\/live$"
   *
   * And also create generic ones like the first few examples as catchalls.
   */

  for (size_t i = 0; i < td->matcher_count; i++) {
    /* each pcre_exec executes on the copy and any modifications must be placed back into copy for the rest of the loop */
    pcre_matcher *m = td->matcher[i];
    if (m != NULL && m->match != NULL) {
      int redo_count = 0;
      int r = 0;
    redo:
      r = pcre_exec(m->match, m->extra, copy, len, 0, 0, ovector, 30);
      if (r <= 0) {
        switch(r) {
        case PCRE_ERROR_NOMATCH      : break;
        case PCRE_ERROR_NULL         : mtevL(mtev_error, "PCRE: Something was null\n");                      break;
        case PCRE_ERROR_BADOPTION    : mtevL(mtev_error, "PCRE: A bad option was passed\n");                 break;
        case PCRE_ERROR_BADMAGIC     : mtevL(mtev_error, "PCRE: Magic number bad (compiled re corrupt?)\n"); break;
        case PCRE_ERROR_UNKNOWN_NODE : mtevL(mtev_error, "PCRE: Something kooky in the compiled re\n");      break;
        case PCRE_ERROR_NOMEMORY     : mtevL(mtev_error, "PCRE: Ran out of memory\n");                       break;
        default                      : mtevL(mtev_error, "PCRE: Unknown error\n");                           break;
        }
      }
      else {
        size_t replace_len = strlen(m->replace);
        char *t = temp;
        /* copy every up to the first capture */
        off_t left = sizeof(temp) - (t - temp);
        assert(ovector[2] < left);
        memcpy(t, copy, ovector[2]);
        t += ovector[2];

        left = sizeof(temp) - (t - temp);
        assert(replace_len < left);
        memcpy(t, m->replace, replace_len);
        t += replace_len;

        left = sizeof(temp) - (t - temp);
        off_t copy_len = len - (ovector[3] - ovector[2]);
        assert(copy_len < left);
        memcpy(t, copy + ovector[3], copy_len);
        t += copy_len;
        *t = '\0';
        strcpy(copy, temp);
      }
      len = strlen(copy);
      if (r > 0) {
        /* we had a match with this RE, run it again and check for more */
        redo_count++;
        if (redo_count < 3) goto redo;
      }
    }
  }
  /* chop off anything after a '?' */
  char *questionmark = strchr(copy, '?');
  if (questionmark != NULL) {
    *questionmark = '\0';
    len = strlen(copy);
  }

  /* chop off any trailing slash */
  if (copy[len - 1] == '/') copy[len - 1] = '\0';
  mtevL(mtev_debug, "\nIncoming: %s\nReplace: %s\n", str, copy);
  return strdup(copy);
}


void
init_path_regex()
{
}

static size_t response_output(void *contents, size_t size, size_t nmemb, void *closure)
{
  mtev_dyn_buffer_t *buffer = (mtev_dyn_buffer_t *)closure;
  if (buffer) {
    mtev_dyn_buffer_add(buffer, (uint8_t *)contents, size * nmemb);
  }
  return size * nmemb;
}


static size_t
to_b64_url(char *b64, size_t len)
{
  size_t new_len = len;
  for (int i = 0; i < new_len; i++) {
    if (b64[i] == '+') {
      b64[i] = '-';
    }
    else if (b64[i] == '/') {
      b64[i] = '_';
    }
    else if (b64[i] == '=') {
      b64[i] = '\0';
      new_len--;
    }
  }
  return new_len;
}

static RSA*
create_private_rsa(const char *private_key)
{
  RSA *rsa = NULL;
  BIO *keybio = BIO_new_mem_buf((void*)private_key, -1);
  if (keybio==NULL) {
    return NULL;
  }
  rsa = PEM_read_bio_RSAPrivateKey(keybio, &rsa, NULL, NULL);
  BIO_set_close(keybio, BIO_NOCLOSE); /* So BIO_free() leaves BUF_MEM alone */
  BIO_free(keybio);
  return rsa;
}

static char *
create_jwt_header(const char *private_key_id, size_t *lenr)
{
  size_t len = snprintf(NULL, 0, "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"%s\"}", private_key_id);
  char *temp = (char *)malloc(len + 1);
  len = snprintf(temp, len+1, "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"%s\"}", private_key_id);
  mtevL(mtev_debug, "header: %s\n", temp);
  size_t encode_len = mtev_b64_encode_len(len);
  char *rval = (char *)malloc(encode_len);
  *lenr = mtev_b64_encode((const unsigned char *)temp, len, rval, encode_len);
  *lenr = to_b64_url(rval, *lenr);
  free(temp);
  return rval;
}

static char *
create_jwt_claim_set(const char *service_account, const char *scope, const char *aud, size_t *lenr)
{
  time_t now = time(NULL);
  char *temp = NULL, *rval = NULL;
  size_t len = snprintf(NULL, 0, "{\"iss\":\"%s\",\"scope\":\"%s\",\"aud\":\"%s\",\"iat\": %d,\"exp\": %d}",
                        service_account, scope, aud, (int)now, (int)(now + 3600));
  temp = (char *)malloc(len + 1);
  len = snprintf(temp, len + 1, "{\"iss\":\"%s\",\"scope\":\"%s\",\"aud\":\"%s\",\"iat\": %d,\"exp\": %d}",
                 service_account, scope, aud, (int)now, (int)(now + 3600));
  mtevL(mtev_debug, "claim set: %s\n", temp);
  size_t encode_len = mtev_b64_encode_len(len);
  rval = (char *)malloc(encode_len);
  *lenr = mtev_b64_encode((const unsigned char *)temp, len, rval, encode_len);
  free(temp);
  *lenr = to_b64_url(rval, *lenr);
  return rval;
}

static char *
create_signed_jwt(const char *private_key_id, const char *private_key, const char *service_account, const char *scope, const char *aud)
{
  /* read the JSON document that contains our keys */
  size_t header_len, claim_len;
  char *header = create_jwt_header(private_key_id, &header_len);
  char *claim_set = create_jwt_claim_set(service_account, scope, aud, &claim_len);

  size_t len_jws = header_len + claim_len + 2; // period + NUL term
  char *jws = (char *)malloc(len_jws);
  size_t jws_len = snprintf(jws, len_jws, "%.*s.%.*s", (int)header_len, header, (int)claim_len, claim_set);
  mtevL(mtev_debug, "%s\n", jws);
  free(header);
  free(claim_set);

  /* sign it */
  RSA* rsa = create_private_rsa(private_key);
  EVP_MD_CTX* rsa_sign_ctx = EVP_MD_CTX_create();
  EVP_PKEY* priKey  = EVP_PKEY_new();
  EVP_PKEY_assign_RSA(priKey, rsa);
  size_t sign_len = 0;
  if (EVP_DigestSignInit(rsa_sign_ctx, NULL, EVP_sha256(), NULL, priKey) <= 0) {
    EVP_MD_CTX_cleanup(rsa_sign_ctx);
    EVP_PKEY_free(priKey); /* also frees rsa */
    free(jws);
    return NULL;
  }
  if (EVP_DigestSignUpdate(rsa_sign_ctx, jws, jws_len) <= 0) {
    EVP_MD_CTX_cleanup(rsa_sign_ctx);
    EVP_PKEY_free(priKey); /* also frees rsa */
    free(jws);
    return NULL;
  }
  if (EVP_DigestSignFinal(rsa_sign_ctx, NULL, &sign_len) <= 0) {
    EVP_MD_CTX_cleanup(rsa_sign_ctx);
    EVP_PKEY_free(priKey); /* also frees rsa */
    free(jws);
    return NULL;
  }
  unsigned char *signature = (unsigned char*)malloc(sign_len);
  if (EVP_DigestSignFinal(rsa_sign_ctx, signature, &sign_len) <= 0) {
    EVP_MD_CTX_cleanup(rsa_sign_ctx);
    EVP_PKEY_free(priKey); /* also frees rsa */
    free(signature);
    free(jws);
    return NULL;
  }
  EVP_MD_CTX_cleanup(rsa_sign_ctx);
  EVP_PKEY_free(priKey); /* also frees rsa */

  size_t signed_jws_encoded_len = mtev_b64_encode_len(sign_len);
  char *b64_jws = (char *)malloc(signed_jws_encoded_len + 10);
  signed_jws_encoded_len = mtev_b64_encode(signature, sign_len, b64_jws, signed_jws_encoded_len + 10);
  signed_jws_encoded_len = to_b64_url(b64_jws, signed_jws_encoded_len);

  char *jwt = NULL;
  size_t len = snprintf(NULL, 0, "%s.%.*s", jws, (int)signed_jws_encoded_len, b64_jws);
  jwt = (char *)malloc(len + 1);
  sprintf(jwt, "%s.%.*s", jws, (int)signed_jws_encoded_len, b64_jws);

  free(b64_jws);
  free(jws);
  free(signature);
  return jwt;
}

const char *
get_oauth2_token(const char *private_key_id, const char *private_key, const char *service_account, const char *scope, const char *aud)
{
  uint64_t nowms = mtev_now_ms();
  if (nowms > last_token_create && nowms - last_token_create > (2400 * 1000)) {
    /* ensure the table exists */
    CURL *curl = curl_easy_init();

    char errors[CURL_ERROR_SIZE] = {0};
    mtev_dyn_buffer_t response_buffer;
    mtev_dyn_buffer_t header_buffer;

    mtev_dyn_buffer_init(&response_buffer);
    mtev_dyn_buffer_init(&header_buffer);

    char *jwt = create_signed_jwt(private_key_id, private_key, service_account, scope, aud);
    size_t auth_fields_len = snprintf(NULL, 0, "grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=%s", jwt);
    char *auth_fields = (char *)malloc(auth_fields_len + 1);
    auth_fields_len = snprintf(auth_fields, auth_fields_len + 1, "grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=%s", jwt);
    mtevL(mtev_debug, "POSTFIELDS: %.*s\n", (int)auth_fields_len, auth_fields);
    /* get the access token if it has expired */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, "https://oauth2.googleapis.com/token");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, response_output);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);
    curl_easy_setopt(curl, CURLOPT_POST, 1L); 
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, auth_fields);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errors);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_EXPECT_100_TIMEOUT_MS, 500L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, response_output);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_buffer);

    CURLcode res = curl_easy_perform(curl);
    free(auth_fields);
    free(jwt);
    curl_easy_cleanup(curl);
    if (res == CURLE_OK) {

      mtev_json_tokener *tok = mtev_json_tokener_new();
      mtev_json_object *json = mtev_json_tokener_parse_ex(tok, (const char *)mtev_dyn_buffer_data(&response_buffer), 
                                                          mtev_dyn_buffer_used(&response_buffer));
      mtev_json_tokener_free(tok);

      free(current_access_token);
      current_access_token = strdup(mtev_json_object_get_string(mtev_json_object_object_get(json, "access_token")));
      last_token_create = mtev_now_ms();
      mtev_dyn_buffer_destroy(&response_buffer);
      mtev_dyn_buffer_destroy(&header_buffer);
      mtev_json_object_put(json);
    } else {
      mtevL(mtev_error, "Unable to get access_token\n");
      mtev_dyn_buffer_destroy(&response_buffer);
      mtev_dyn_buffer_destroy(&header_buffer);
      return NULL;
    }
  }
  return current_access_token;
}

