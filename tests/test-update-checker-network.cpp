#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <functional>
#include <memory>

#include <gtest/gtest.h>

#include "vdoninja-update-checker.h"

using namespace vdoninja;

namespace
{
// Fault injection is confined to this test process. No hosts-file or system proxy changes.
class ScopedEnvironment
{
public:
	ScopedEnvironment(const char *name, const QByteArray &value)
	    : name_(name), existed_(qEnvironmentVariableIsSet(name)), previous_(qgetenv(name))
	{
		qputenv(name, value);
	}
	~ScopedEnvironment()
	{
		if (existed_)
			qputenv(name_, previous_);
		else
			qunsetenv(name_);
	}

private:
	const char *name_;
	bool existed_;
	QByteArray previous_;
};

class UpdateNetworkTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		if (!QCoreApplication::instance()) {
			static int argc = 1;
			static char name[] = "vdoninja-update-network-tests";
			static char *argv[] = {name, nullptr};
			static QCoreApplication app(argc, argv);
		}
		server_ = std::make_unique<QTcpServer>();
		ASSERT_TRUE(server_->listen(QHostAddress::LocalHost));
		const auto proxy = "http://127.0.0.1:" + QByteArray::number(server_->serverPort());
		upperProxy_ = std::make_unique<ScopedEnvironment>("HTTPS_PROXY", proxy);
		lowerProxy_ = std::make_unique<ScopedEnvironment>("https_proxy", proxy);
		upperBypass_ = std::make_unique<ScopedEnvironment>("NO_PROXY", "");
		lowerBypass_ = std::make_unique<ScopedEnvironment>("no_proxy", "");
		QObject::connect(server_.get(), &QTcpServer::newConnection, server_.get(), [this] {
			auto *socket = server_->nextPendingConnection();
			QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
				request_ += socket->readAll();
				if (!requestHandled_ && request_.contains("\r\n\r\n")) {
					requestHandled_ = true;
					if (respond_)
						respond_(socket);
				}
			});
		});
	}

	void waitForResult(VDONinjaUpdateChecker &checker, int timeoutMs = 17000)
	{
		QEventLoop loop;
		QObject::connect(&checker, &VDONinjaUpdateChecker::statusChanged, &loop, [&] {
			if (checker.result().status != UpdateCheckStatus::Checking)
				loop.quit();
		});
		QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
		loop.exec();
		EXPECT_TRUE(request_.startsWith("CONNECT api.github.com:443 HTTP/1.1\r\n"));
		EXPECT_EQ(checker.result().status, UpdateCheckStatus::Unavailable);
	}

	std::unique_ptr<QTcpServer> server_;
	std::unique_ptr<ScopedEnvironment> upperProxy_, lowerProxy_, upperBypass_, lowerBypass_;
	QByteArray request_;
	bool requestHandled_ = false;
	std::function<void(QTcpSocket *)> respond_;
};
} // namespace

TEST_F(UpdateNetworkTest, RealCurlRejectsProxyErrorsWithoutAnUpdate)
{
	respond_ = [](QTcpSocket *socket) {
		socket->write("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
		socket->disconnectFromHost();
	};
	VDONinjaUpdateChecker checker("1.1.73", {});
	checker.checkForUpdates();
	waitForResult(checker);
}

TEST_F(UpdateNetworkTest, RealCurlRejectsBrokenTlsTunnel)
{
	respond_ = [](QTcpSocket *socket) {
		socket->write("HTTP/1.1 200 Connection established\r\n\r\nThis is not a TLS handshake");
		socket->disconnectFromHost();
	};
	VDONinjaUpdateChecker checker("1.1.73", {});
	checker.checkForUpdates();
	waitForResult(checker);
}

TEST_F(UpdateNetworkTest, RealCurlTimesOutWithoutBlockingTheEventLoop)
{
	VDONinjaUpdateChecker checker("1.1.73", {});
	int heartbeats = 0;
	QTimer heartbeat;
	QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
	heartbeat.start(50);
	QElapsedTimer elapsed;
	elapsed.start();
	checker.checkForUpdates();
	waitForResult(checker);
	EXPECT_LT(elapsed.elapsed(), 16500);
	EXPECT_GT(heartbeats, 50);
}

TEST_F(UpdateNetworkTest, ShutdownDuringRealRequestIsPromptAndSilent)
{
	VDONinjaUpdateChecker checker("1.1.73", {});
	bool canceled = false;
	respond_ = [&](QTcpSocket *) {
		QElapsedTimer elapsed;
		elapsed.start();
		checker.shutdown();
		EXPECT_LT(elapsed.elapsed(), 1000);
		canceled = true;
	};
	checker.checkForUpdates();
	int notifications = 0;
	QObject::connect(&checker, &VDONinjaUpdateChecker::statusChanged, [&] { ++notifications; });
	QEventLoop loop;
	QTimer::singleShot(1200, &loop, &QEventLoop::quit);
	loop.exec();
	EXPECT_TRUE(canceled);
	EXPECT_EQ(notifications, 0);
}
