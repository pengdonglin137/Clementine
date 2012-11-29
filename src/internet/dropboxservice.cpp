#include "dropboxservice.h"

#include <QFileInfo>

#include <qjson/parser.h>

#include "core/application.h"
#include "core/logging.h"
#include "core/network.h"
#include "core/player.h"
#include "core/waitforsignal.h"
#include "internet/dropboxauthenticator.h"
#include "internet/dropboxurlhandler.h"

const char* DropboxService::kServiceName = "Dropbox";
const char* DropboxService::kSettingsGroup = "Dropbox";

namespace {

static const char* kServiceId = "dropbox";

static const char* kMetadataEndpoint =
    "https://api.dropbox.com/1/metadata/dropbox/";
static const char* kMediaEndpoint =
    "https://api.dropbox.com/1/media/dropbox/";

}  // namespace

DropboxService::DropboxService(Application* app, InternetModel* parent)
    : CloudFileService(
        app, parent,
        kServiceName, kServiceId,
        QIcon(":/providers/dropbox.png"),
        SettingsDialog::Page_Dropbox),
      network_(new NetworkAccessManager(this)) {
  QSettings settings;
  settings.beginGroup(kSettingsGroup);
  access_token_ = settings.value("access_token").toString();
  access_token_secret_ = settings.value("access_token_secret").toString();
  app->player()->RegisterUrlHandler(new DropboxUrlHandler(this, this));
}

bool DropboxService::has_credentials() const {
  return !access_token_.isEmpty();
}

void DropboxService::Connect() {
  if (has_credentials()) {
    RequestFileList("");
  } else {
    ShowSettingsDialog();
  }
}

void DropboxService::AuthenticationFinished(DropboxAuthenticator* authenticator) {
  authenticator->deleteLater();

  access_token_ = authenticator->access_token();
  access_token_secret_ = authenticator->access_token_secret();

  QSettings settings;
  settings.beginGroup(kSettingsGroup);

  settings.setValue("access_token", access_token_);
  settings.setValue("access_token_secret", access_token_secret_);
  settings.setValue("name", authenticator->name());

  emit Connected();

  RequestFileList("");
}

QByteArray DropboxService::GenerateAuthorisationHeader() {
  return DropboxAuthenticator::GenerateAuthorisationHeader(
      access_token_,
      access_token_secret_);
}

void DropboxService::RequestFileList(const QString& path) {
  QUrl url(QString(kMetadataEndpoint) + path);
  QNetworkRequest request(url);
  request.setRawHeader("Authorization", GenerateAuthorisationHeader());

  QNetworkReply* reply = network_->get(request);
  NewClosure(reply, SIGNAL(finished()),
             this, SLOT(RequestFileListFinished(QNetworkReply*)), reply);
}

namespace {

bool IsSupportedMimeType(const QString& mime_type) {
  return mime_type == "audio/ogg" ||
         mime_type == "audio/mpeg";
}

}  // namespace

void DropboxService::RequestFileListFinished(QNetworkReply* reply) {
  reply->deleteLater();

  QJson::Parser parser;
  QVariantMap response = parser.parse(reply).toMap();
  QVariantList contents = response["contents"].toList();
  foreach (const QVariant& c, contents) {
    QVariantMap item = c.toMap();
    const bool directory = item["is_dir"].toBool();
    if (directory) {
      RequestFileList(item["path"].toString());
    } else if (IsSupportedMimeType(item["mime_type"].toString())) {
      qLog(Debug) << "Found:" << item["path"].toString();
      QUrl url;
      url.setScheme("dropbox");
      url.setPath(item["path"].toString());
      QNetworkReply* reply = FetchContentUrl(url);
      NewClosure(reply, SIGNAL(finished()),
                 this, SLOT(FetchContentUrlFinished(QNetworkReply*, QVariantMap)),
                 reply, item);
    }
  }
}

QNetworkReply* DropboxService::FetchContentUrl(const QUrl& url) {
  QUrl request_url(QString(kMediaEndpoint) + url.path());
  QNetworkRequest request(request_url);
  request.setRawHeader("Authorization", GenerateAuthorisationHeader());
  return network_->post(request, QByteArray());
}

void DropboxService::FetchContentUrlFinished(
    QNetworkReply* reply, const QVariantMap& data) {
  reply->deleteLater();
  QJson::Parser parser;
  QVariantMap response = parser.parse(reply).toMap();
  QFileInfo info(data["path"].toString());
  qLog(Debug) << response["url"].toUrl()
              << info.fileName()
              << data["bytes"].toInt()
              << data["mime_type"].toString();
  TagReaderClient::ReplyType* tag_reply = app_->tag_reader_client()->ReadCloudFile(
      response["url"].toUrl(),
      info.fileName(),
      data["bytes"].toInt(),
      data["mime_type"].toString(),
      QString::null);
  NewClosure(tag_reply, SIGNAL(Finished(bool)),
      this, SLOT(ReadTagsFinished(TagReaderClient::ReplyType*,QVariantMap)),
      tag_reply, response);
}

void DropboxService::ReadTagsFinished(
    TagReaderClient::ReplyType* reply, const QVariantMap& file) {
  qLog(Debug) << reply->message().DebugString().c_str();
}

QUrl DropboxService::GetStreamingUrlFromSongId(const QUrl& url) {
  QNetworkReply* reply = FetchContentUrl(url);
  WaitForSignal(reply, SIGNAL(finished()));

  QJson::Parser parser;
  QVariantMap response = parser.parse(reply).toMap();
  return response["url"].toUrl();
}