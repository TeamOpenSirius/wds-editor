#include "wds/ui/qt/system_proxy.hpp"

#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QStringList>

namespace wds::ui {
namespace {

const char* const kProxyEnvironmentVariables[] = {
    "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy",
};

QString proxy_scheme(QNetworkProxy::ProxyType type) {
  switch (type) {
    case QNetworkProxy::HttpProxy:
    case QNetworkProxy::HttpCachingProxy:
    case QNetworkProxy::FtpCachingProxy:
      return QStringLiteral("http");
    case QNetworkProxy::Socks5Proxy:
      return QStringLiteral("socks5");
    default:
      return {};
  }
}

bool usable_proxy(const QNetworkProxy& proxy) {
  return !proxy_scheme(proxy.type()).isEmpty() && !proxy.hostName().isEmpty() && proxy.port() > 0;
}

// The system backend is queried per request, so PAC rules that route different
// hosts through different proxies keep working.
class SystemProxyFactory final : public QNetworkProxyFactory {
 public:
  QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query) override {
    return QNetworkProxyFactory::systemProxyForQuery(query);
  }
};

}  // namespace

QNetworkProxy system_proxy_for(const QUrl& url) {
  const QList<QNetworkProxy> proxies =
      QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(url));
  for (const QNetworkProxy& proxy : proxies) {
    if (usable_proxy(proxy)) return proxy;
  }
  return QNetworkProxy(QNetworkProxy::NoProxy);
}

QString proxy_to_url(const QNetworkProxy& proxy) {
  const QString scheme = proxy_scheme(proxy.type());
  if (scheme.isEmpty() || proxy.hostName().isEmpty() || proxy.port() <= 0) return {};

  QUrl url;
  url.setScheme(scheme);
  url.setHost(proxy.hostName());
  url.setPort(proxy.port());
  if (!proxy.user().isEmpty()) {
    url.setUserName(proxy.user());
    if (!proxy.password().isEmpty()) url.setPassword(proxy.password());
  }
  return QString::fromLatin1(
      url.toEncoded(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment));
}

void apply_proxy_environment(QProcessEnvironment& env, const QNetworkProxy& proxy) {
  const QString proxy_url = proxy_to_url(proxy);
  if (proxy_url.isEmpty()) return;

  for (const char* name : kProxyEnvironmentVariables) env.remove(QLatin1String(name));
  if (proxy.type() == QNetworkProxy::Socks5Proxy) {
    // pip/requests only speak SOCKS with PySocks, which the stem runtime does not
    // install. ALL_PROXY still lets curl and urllib-based callers use it; setting
    // HTTP(S)_PROXY to socks5:// would make requests reject the scheme outright.
    env.insert(QStringLiteral("ALL_PROXY"), proxy_url);
    env.insert(QStringLiteral("all_proxy"), proxy_url);
    return;
  }
  for (const char* name : kProxyEnvironmentVariables) {
    env.insert(QLatin1String(name), proxy_url);
  }
}

void apply_system_proxy(QProcessEnvironment& env, const QUrl& url) {
  const QNetworkProxy proxy = system_proxy_for(url);
  if (proxy.type() == QNetworkProxy::NoProxy) return;
  apply_proxy_environment(env, proxy);
}

void use_system_proxy(QNetworkAccessManager* manager) {
  if (manager != nullptr) manager->setProxyFactory(new SystemProxyFactory);
}

}  // namespace wds::ui
