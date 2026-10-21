#include "wds/ui/qt/system_proxy.hpp"

#include <QCoreApplication>
#include <QNetworkAccessManager>
#include <QProcessEnvironment>
#include <QUrl>

#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "FAIL %s\n", message);
  ++failures;
}

void test_http_proxy_url() {
  const QNetworkProxy proxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 7890);
  expect(wds::ui::proxy_to_url(proxy) == QStringLiteral("http://127.0.0.1:7890"),
         "http proxy url");
}

void test_proxy_url_encodes_credentials() {
  QNetworkProxy proxy(QNetworkProxy::HttpProxy, QStringLiteral("proxy.example"), 8080);
  proxy.setUser(QStringLiteral("user name"));
  proxy.setPassword(QStringLiteral("p@ss"));
  expect(wds::ui::proxy_to_url(proxy) ==
             QStringLiteral("http://user%20name:p%40ss@proxy.example:8080"),
         "credentials are percent encoded");
}

void test_socks_proxy_url() {
  const QNetworkProxy proxy(QNetworkProxy::Socks5Proxy, QStringLiteral("::1"), 1080);
  expect(wds::ui::proxy_to_url(proxy) == QStringLiteral("socks5://[::1]:1080"),
         "socks proxy url brackets ipv6");
}

void test_unusable_proxy_has_no_url() {
  const QNetworkProxy none(QNetworkProxy::NoProxy);
  const QNetworkProxy no_host(QNetworkProxy::HttpProxy, QString(), 8080);
  const QNetworkProxy no_port(QNetworkProxy::HttpProxy, QStringLiteral("proxy.example"), 0);
  expect(wds::ui::proxy_to_url(none).isEmpty(), "no proxy has no url");
  expect(wds::ui::proxy_to_url(no_host).isEmpty(), "proxy without host has no url");
  expect(wds::ui::proxy_to_url(no_port).isEmpty(), "proxy without port has no url");
}

void test_http_proxy_environment() {
  QProcessEnvironment env;
  env.insert(QStringLiteral("HTTPS_PROXY"), QStringLiteral("http://old.example:1"));
  env.insert(QStringLiteral("NO_PROXY"), QStringLiteral("localhost"));
  const QNetworkProxy proxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 7890);
  wds::ui::apply_proxy_environment(env, proxy);
  for (const char* name : {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy",
                           "https_proxy", "all_proxy"}) {
    expect(env.value(QLatin1String(name)) == QStringLiteral("http://127.0.0.1:7890"),
           "http proxy exported to every case");
  }
  expect(env.value(QStringLiteral("NO_PROXY")) == QStringLiteral("localhost"),
         "existing no_proxy is preserved");
}

void test_socks_proxy_environment() {
  QProcessEnvironment env;
  env.insert(QStringLiteral("HTTP_PROXY"), QStringLiteral("http://old.example:1"));
  env.insert(QStringLiteral("https_proxy"), QStringLiteral("http://old.example:1"));
  const QNetworkProxy proxy(QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"), 1080);
  wds::ui::apply_proxy_environment(env, proxy);
  expect(env.value(QStringLiteral("ALL_PROXY")) == QStringLiteral("socks5://127.0.0.1:1080"),
         "socks proxy exported as all_proxy");
  expect(env.value(QStringLiteral("all_proxy")) == QStringLiteral("socks5://127.0.0.1:1080"),
         "socks proxy exported as lowercase all_proxy");
  // requests rejects socks5:// in HTTPS_PROXY without PySocks; do not put it there.
  expect(env.value(QStringLiteral("HTTP_PROXY")).isEmpty(), "socks clears http_proxy");
  expect(env.value(QStringLiteral("https_proxy")).isEmpty(), "socks clears https_proxy");
}

void test_unusable_proxy_leaves_environment_untouched() {
  QProcessEnvironment env;
  env.insert(QStringLiteral("HTTPS_PROXY"), QStringLiteral("http://env.example:8080"));
  wds::ui::apply_proxy_environment(env, QNetworkProxy(QNetworkProxy::NoProxy));
  expect(env.value(QStringLiteral("HTTPS_PROXY")) == QStringLiteral("http://env.example:8080"),
         "no proxy keeps environment proxy");
}

void test_system_proxy_is_usable_when_present() {
  const QNetworkProxy proxy = wds::ui::system_proxy_for(QUrl(QStringLiteral("https://pypi.org/")));
  expect(proxy.type() == QNetworkProxy::NoProxy || !wds::ui::proxy_to_url(proxy).isEmpty(),
         "system proxy resolves to a usable url");
}

void test_manager_queries_system_proxy() {
  QNetworkAccessManager manager;
  wds::ui::use_system_proxy(&manager);
  expect(manager.proxyFactory() != nullptr, "manager gets a proxy factory");
  wds::ui::use_system_proxy(nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  test_http_proxy_url();
  test_proxy_url_encodes_credentials();
  test_socks_proxy_url();
  test_unusable_proxy_has_no_url();
  test_http_proxy_environment();
  test_socks_proxy_environment();
  test_unusable_proxy_leaves_environment_untouched();
  test_system_proxy_is_usable_when_present();
  test_manager_queries_system_proxy();
  if (failures != 0) {
    std::fprintf(stderr, "%d system proxy tests failed\n", failures);
    return 1;
  }
  return 0;
}
