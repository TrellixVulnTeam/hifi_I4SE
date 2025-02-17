//
//  LoginDialog.h
//  interface/src/ui
//
//  Created by Bradley Austin Davis on 2015/04/14
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#pragma once

#ifndef hifi_LoginDialog_h
#define hifi_LoginDialog_h

#include <OffscreenQmlDialog.h>

class QNetworkReply;

extern const QUrl OVERLAY_LOGIN_DIALOG;

class LoginDialog : public OffscreenQmlDialog {
    Q_OBJECT
    Q_PROPERTY(bool isLogIn READ getIsLogIn WRITE setIsLogIn)
    HIFI_QML_DECL

public:
    static void toggleAction();

    LoginDialog(QQuickItem* parent = nullptr);

    ~LoginDialog();

    static void showWithSelection();

signals:
    void handleLoginCompleted();
    void handleLoginFailed();

    void handleLinkCompleted();
    void handleLinkFailed(QString error);

    void handleCreateCompleted();
    void handleCreateFailed(QString error);

    void handleSignupCompleted();
    void handleSignupFailed(QString errorString);

    // occurs upon dismissing the encouraging log in.
    void dismissedLoginDialog();

    void focusEnabled();
    void focusDisabled();

public slots:
    void linkCompleted(QNetworkReply* reply);
    void linkFailed(QNetworkReply* reply);

    void createCompleted(QNetworkReply* reply);
    void createFailed(QNetworkReply* reply);

    void signupCompleted(QNetworkReply* reply);
    void signupFailed(QNetworkReply* reply);

protected slots:
    Q_INVOKABLE void dismissLoginDialog();

    Q_INVOKABLE bool isSteamRunning() const;
    Q_INVOKABLE bool isOculusStoreRunning() const;

    Q_INVOKABLE void login(const QString& username, const QString& password) const;
    Q_INVOKABLE void loginThroughSteam();
    Q_INVOKABLE void linkSteam();
    Q_INVOKABLE void createAccountFromSteam(QString username = QString());

    Q_INVOKABLE void signup(const QString& email, const QString& username, const QString& password);

    Q_INVOKABLE void openUrl(const QString& url) const;

    Q_INVOKABLE bool getLoginDialogPoppedUp() const;

private:
    bool getIsLogIn() const { return _isLogIn; }
    void setIsLogIn(const bool isLogIn) { _isLogIn = isLogIn; }

    bool _isLogIn{ false };
};

#endif // hifi_LoginDialog_h
