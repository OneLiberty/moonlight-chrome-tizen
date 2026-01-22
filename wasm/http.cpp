#include "moonlight_wasm.hpp"

#include <errors.h>
#include <http.h>
#include <string.h>

#include <mkcert.h>
#include <openssl/bio.h>
#include <openssl/pem.h>

#include <curl/curl.h>

X509* g_Cert;
EVP_PKEY* g_PrivateKey;
char* g_UniqueId;
char* g_CertHex;

MessageResult MoonlightInstance::MakeCert() {
  CERT_KEY_PAIR certKeyPair = mkcert_generate();

  BIO* bio = BIO_new(BIO_s_mem());

  PEM_write_bio_X509(bio, certKeyPair.x509);
  BUF_MEM* mem = NULL;
  BIO_get_mem_ptr(bio, &mem);

  std::string cert(mem->data, mem->length);

  BIO_free(bio);

  BIO* biokey = BIO_new(BIO_s_mem());
  PEM_write_bio_PrivateKey(biokey, certKeyPair.pkey, NULL, NULL, 0, NULL, NULL);
  BIO_get_mem_ptr(biokey, &mem);

  std::string pkey(mem->data, mem->length);

  BIO_free(biokey);

  emscripten::val ret = emscripten::val::object();
  ret.set("cert", emscripten::val(cert));
  ret.set("privateKey", emscripten::val(pkey));

  return MessageResult::Resolve(ret);
}

LoadResult MoonlightInstance::LoadCert(const char* certStr, const char* keyStr) {
  char* _certStr = strdup(certStr);
  char* _keyStr = strdup(keyStr);

  BIO* bio = BIO_new_mem_buf(_certStr, -1);
  if (!(g_Cert = PEM_read_bio_X509(bio, NULL, NULL, NULL))) {
    return LoadResult::CertErr;
  }
  BIO_free_all(bio);

  bio = BIO_new_mem_buf(_keyStr, -1);
  if (!(g_PrivateKey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL))) {
    return LoadResult::PrivateKeyErr;
  }
  BIO_free_all(bio);

  // Convert the PEM cert to hex
  g_CertHex = reinterpret_cast<char*>(malloc((strlen(certStr) * 2) + 1));
  for (size_t i = 0; i < strlen(certStr); i++) {
    sprintf(&g_CertHex[i * 2], "%02x", certStr[i]);
  }

  free(_certStr);
  free(_keyStr);

  return LoadResult::Success;
}

MessageResult MoonlightInstance::HttpInit(std::string cert, std::string privateKey, std::string myUniqueId) {
  LoadResult res = LoadResult::Success;
  res = LoadCert(cert.c_str(), privateKey.c_str());
  if (res == LoadResult::CertErr) {
    return MessageResult::Reject(
        emscripten::val(std::string("Error loading cert into memory")));
  } else if (res == LoadResult::PrivateKeyErr) {
    return MessageResult::Reject(
        emscripten::val(std::string("Error loading private key into memory")));
  }
  res = LoadCert(cert.c_str(), privateKey.c_str());
  if (res == LoadResult::CertErr) {
    return MessageResult::Reject(
        emscripten::val(std::string("Error loading cert into memory")));
  } else if (res == LoadResult::PrivateKeyErr) {
    return MessageResult::Reject(
        emscripten::val(std::string("Error loading private key into memory")));
  }

  g_UniqueId = strdup(myUniqueId.c_str());

  curl_global_init(CURL_GLOBAL_DEFAULT);

  return MessageResult::Resolve();
}

void MoonlightInstance::OpenUrl_private(int callbackId, std::string url, std::string ppk, bool binaryResponse) {
  PHTTP_DATA data = http_create_data();
  int err;

  if (data == NULL) {
    PostPromiseMessage(callbackId, "reject", "Error when creating data buffer.");
    return;
  }

  err = http_request(url.c_str(), ppk.c_str(), data);
  if (err) {
    http_free_data(data);
    PostPromiseMessage(callbackId, "reject", std::to_string(err));
    return;
  }

  if (binaryResponse) {
    std::vector<uint8_t> response;
    response.resize(data->size);
    memcpy(response.data(), data->memory, data->size);

    http_free_data(data);

    PostPromiseMessage(callbackId, "resolve", response);
  } else {
    std::string response{data->memory, data->size};

    http_free_data(data);

    PostPromiseMessage(callbackId, "resolve", response);
  }
}

void MoonlightInstance::OpenUrl(int callbackId, std::string url, std::string ppk, bool binaryResponse) {
  m_Dispatcher.post_job(std::bind(&MoonlightInstance::OpenUrl_private, this, callbackId, url, ppk, binaryResponse), false);
}

MessageResult makeCert() { return g_Instance->MakeCert(); }

MessageResult httpInit(std::string cert, std::string privateKey, std::string myUniqueId) {
  return g_Instance->HttpInit(cert, privateKey, myUniqueId);
}

void openUrl(int callbackId, std::string url, emscripten::val ppk, bool binaryResponse) {
  std::string ppkstr = "";
  if (ppk != emscripten::val::null()) {
    ppkstr = ppk.as<std::string>();
  }
  g_Instance->OpenUrl(callbackId, url, ppkstr, binaryResponse);
}

EMSCRIPTEN_BINDINGS(http) {
  emscripten::function("makeCert", &makeCert);
  emscripten::function("httpInit", &httpInit);
  emscripten::function("openUrl", &openUrl);
}
