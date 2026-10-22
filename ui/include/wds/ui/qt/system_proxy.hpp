#pragma once

#include <QNetworkProxy>
#include <QProcessEnvironment>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;

namespace wds::ui {

// Resolve the proxy the OS would use for `url`, using the same platform query as
// the Qt network stack (including per-protocol entries and PAC where supported).
// Returns NoProxy when the OS has no proxy or bypasses it.
QNetworkProxy system_proxy_for(const QUrl& url);

// "http://user:pass@host:port" / "socks5://host:port", or empty when proxy is
// unusable. Suitable for the standard *_proxy environment variables.
QString proxy_to_url(const QNetworkProxy& proxy);

// Export `proxy` to the proxy variables understood by pip, urllib and torch.hub.
// Existing proxy variables are replaced so they cannot disagree with Qt's choice.
// An unusable proxy leaves the environment untouched.
void apply_proxy_environment(QProcessEnvironment& env, const QNetworkProxy& proxy);

// Same as above after resolving the OS proxy for `url`. When the OS has no proxy
// for that URL, any proxy already present in `env` is preserved.
void apply_system_proxy(QProcessEnvironment& env, const QUrl& url);

// Make a QNetworkAccessManager resolve the OS proxy for every request instead of
// relying on the application proxy. This is what QNetworkAccessManager does by
// default, but stating it here keeps the download independent of global Qt state.
void use_system_proxy(QNetworkAccessManager* manager);

}  // namespace wds::ui
