/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026  qBitFusion contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "onboardingdialog.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "base/hybrid/hybridmanager.h"
#include "base/hybrid/hybridtypes.h"
#include "base/hybrid/providerrealdebrid.h"
#include "base/hybrid/securestore.h"
#include "base/settingvalue.h"
#include "gui/hybrid/hybridwidgets.h"
#include "gui/modern/modernappearance.h"
#include "gui/modern/modernwidgets.h"
#include "onboardingart.h"

using namespace Qt::Literals::StringLiterals;

namespace
{
    constexpr int CONTENT_WIDTH = 540;
    constexpr int FEATURE_WIDTH = CONTENT_WIDTH - 40;
    constexpr int FEATURE_TEXT_WIDTH = FEATURE_WIDTH - 28 - 16;
    constexpr int MODE_TEXT_WIDTH = 180;
    const QString KEY_TOUR_VERSION = u"GUI/OnboardingVersion"_s;

    enum Page
    {
        PAGE_WELCOME,
        PAGE_HYBRID,
        PAGE_CONNECT,
        PAGE_LINKS,
        PAGE_DONE,

        PAGE_COUNT
    };

    qreal screenRatio()
    {
        return qApp->devicePixelRatio();
    }

    QLabel *titleLabel(const QString &text, QWidget *parent)
    {
        auto *label = new QLabel(text, parent);
        QFont font = label->font();
        font.setPointSizeF(font.pointSizeF() + 7);
        font.setWeight(QFont::DemiBold);
        label->setFont(font);
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        return label;
    }

    QLabel *bodyLabel(const QString &text, QWidget *parent)
    {
        auto *label = new QLabel(text, parent);
        QFont font = label->font();
        font.setPointSizeF(font.pointSizeF() + 0.5);
        label->setFont(font);
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        label->setFixedWidth(CONTENT_WIDTH);
        Modern::markAsHint(label);
        return label;
    }

    QLabel *hintLabel(const QString &text, QWidget *parent, const Qt::Alignment alignment = Qt::AlignCenter)
    {
        auto *label = new QLabel(text, parent);
        label->setAlignment(alignment);
        label->setWordWrap(true);
        Modern::markAsHint(label);
        return label;
    }

    // A wrapped label inside an aligned layout cell gets no height-for-width, so its width is fixed here.
    QLabel *wrapTo(QLabel *label, const int width)
    {
        label->setFixedWidth(width);
        label->setMinimumHeight(label->heightForWidth(width));
        return label;
    }

    QLabel *glyphLabel(const QString &iconId, const QColor &color, const int size, QWidget *parent)
    {
        auto *label = new QLabel(parent);
        label->setPixmap(Onboarding::glyph(iconId, color, size, screenRatio()));
        label->setFixedSize(size, size);
        return label;
    }

    // Icon, a short title and one sentence: the "what's new" row of the welcome page.
    QWidget *featureRow(const QString &iconId, const QString &title, const QString &text, QWidget *parent)
    {
        auto *row = new QWidget(parent);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(16);
        layout->addWidget(glyphLabel(iconId, Onboarding::accentColor(), 28, row), 0, Qt::AlignTop);

        auto *texts = new QVBoxLayout;
        texts->setSpacing(2);
        auto *heading = new QLabel(title, row);
        QFont font = heading->font();
        font.setWeight(QFont::DemiBold);
        heading->setFont(font);
        texts->addWidget(heading);
        texts->addWidget(wrapTo(hintLabel(text, row, (Qt::AlignLeft | Qt::AlignTop)), FEATURE_TEXT_WIDTH));
        layout->addLayout(texts, 1);
        return row;
    }

    // A page: illustration, title, explanation, and anything else under them, centered.
    QWidget *makePage(QWidget *art, const QString &title, const QString &body, QWidget *pageParent)
    {
        auto *page = new QWidget(pageParent);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(36, 26, 36, 8);
        layout->setSpacing(0);
        if (art)
        {
            art->setParent(page);
            layout->addWidget(art);
            layout->addSpacing(18);
        }
        layout->addWidget(titleLabel(title, page));
        layout->addSpacing(10);
        layout->addWidget(bodyLabel(body, page), 0, Qt::AlignHCenter);
        return page;
    }

    QVBoxLayout *pageLayout(QWidget *page)
    {
        return static_cast<QVBoxLayout *>(page->layout());
    }

    QString policyDescription(const Hybrid::TransportPolicy policy)
    {
        switch (policy)
        {
        case Hybrid::TransportPolicy::DebridOnly:
            return OnboardingDialog::tr("Private: only your service, never the torrent swarm.");
        case Hybrid::TransportPolicy::DebridPreferred:
            return OnboardingDialog::tr("Your service first; the swarm fills gaps once you allow it.");
        case Hybrid::TransportPolicy::TorrentOnly:
            return OnboardingDialog::tr("Peers only, exactly like qBittorrent.");
        }
        return {};
    }
}

bool Onboarding::shouldShow()
{
    return SettingValue<int>(KEY_TOUR_VERSION).get(0) < TOUR_VERSION;
}

void Onboarding::markShown()
{
    SettingValue<int> stored {KEY_TOUR_VERSION};
    stored = TOUR_VERSION;
}

OnboardingDialog::OnboardingDialog(QWidget *parent)
    : QDialog(parent)
    , m_countdown {new QTimer(this)}
{
    setWindowTitle(tr("Welcome to qBitFusion"));
    setFixedSize(720, 640);

    if (auto *manager = Hybrid::HybridManager::instance())
        m_provider = manager->realDebrid();

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(createWelcomePage());
    m_pages->addWidget(createHybridPage());
    m_connectPage = m_pages->addWidget(createConnectPage());
    m_pages->addWidget(createLinksPage());
    m_pages->addWidget(createDonePage());

    // Footer: where you are, and the way on.
    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setFixedHeight(1);
    QPalette linePalette = line->palette();
    linePalette.setColor(QPalette::WindowText, Onboarding::hairlineColor(this));
    line->setPalette(linePalette);

    m_dots = new Onboarding::PageDots(PAGE_COUNT, this);
    connect(m_dots, &Onboarding::PageDots::clicked, this, &OnboardingDialog::goTo);

    m_skip = new QPushButton(tr("Skip tour"), this);
    m_skip->setFlat(true);
    m_skip->setCursor(Qt::PointingHandCursor);
    m_skip->setAutoDefault(false);
    connect(m_skip, &QPushButton::clicked, this, &QDialog::reject);

    m_back = new QPushButton(tr("Back"), this);
    m_back->setAutoDefault(false);
    connect(m_back, &QPushButton::clicked, this, [this] { goTo(m_pages->currentIndex() - 1); });

    m_next = new QPushButton(tr("Continue"), this);
    m_next->setObjectName(u"qbfPrimaryButton"_s);
    m_next->setDefault(true);
    m_next->setMinimumWidth(120);
    connect(m_next, &QPushButton::clicked, this, [this]
    {
        if (m_pages->currentIndex() == (PAGE_COUNT - 1))
            accept();
        else
            goTo(m_pages->currentIndex() + 1);
    });

    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(24, 14, 24, 16);
    footer->setSpacing(8);
    footer->addWidget(m_dots, 0, Qt::AlignVCenter);
    footer->addStretch();
    footer->addWidget(m_skip);
    footer->addWidget(m_back);
    footer->addWidget(m_next);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_pages, 1);
    layout->addWidget(line);
    layout->addLayout(footer);

    connect(m_countdown, &QTimer::timeout, this, &OnboardingDialog::tick);
    if (m_provider)
    {
        connect(m_provider, &Hybrid::RealDebridProvider::deviceCodeReady, this, &OnboardingDialog::onDeviceCode);
        connect(m_provider, &Hybrid::RealDebridProvider::authorizationFinished, this, &OnboardingDialog::onSignInFinished);
        connect(m_provider, &Hybrid::Provider::accountStateChanged, this, [this]
        {
            if (m_signingIn)
                return;
            m_checking = false;
            refreshAccount(false);
        });
    }
    if (auto *manager = Hybrid::HybridManager::instance())
    {
        connect(manager, &Hybrid::HybridManager::settingsChanged, this, [this]
        {
            if (!m_signingIn)
                refreshAccount(false);
        });
    }

    // An existing connection is checked again, so that "connected" means the account answers now.
    if (m_provider && (m_provider->accountState() == Hybrid::AccountState::Connected))
    {
        m_checking = true;
        m_provider->refreshAccountInfo();
    }
    refreshAccount(false);
    goTo(PAGE_WELCOME);
}

OnboardingDialog::~OnboardingDialog()
{
    cancelSignIn();
}

void OnboardingDialog::done(const int result)
{
    cancelSignIn();
    Onboarding::markShown();
    QDialog::done(result);
}

// ------------------------------------------------------------------------------- pages

QWidget *OnboardingDialog::createWelcomePage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(36, 34, 36, 8);
    layout->setSpacing(0);

    auto *mark = new QLabel(page);
    mark->setPixmap(QIcon(u":/qbitfusion/brand/qbitfusion.png"_s).pixmap(QSize(76, 76), screenRatio()));
    mark->setAlignment(Qt::AlignCenter);
    layout->addWidget(mark);
    layout->addSpacing(16);
    layout->addWidget(titleLabel(tr("Welcome to qBitFusion"), page));
    layout->addSpacing(8);
    layout->addWidget(bodyLabel(tr("Everything qBittorrent does, plus a few things it doesn't. Here's what's new."), page), 0, Qt::AlignHCenter);
    layout->addSpacing(28);

    auto *features = new QWidget(page);
    features->setFixedWidth(FEATURE_WIDTH);
    auto *list = new QVBoxLayout(features);
    list->setContentsMargins(0, 0, 0, 0);
    list->setSpacing(20);
    list->addWidget(featureRow(u"network-server"_s, tr("Your debrid service, inside torrents")
            , tr("Download a torrent from Real-Debrid over HTTPS, from the swarm, or both. Every piece is checked."), features));
    list->addWidget(featureRow(u"insert-link"_s, tr("Plain file links, too")
            , tr("Paste a link from Stremio or your debrid service, and the file downloads on the new Links page."), features));
    list->addWidget(featureRow(u"security-high"_s, tr("Private when you want it")
            , tr("\"Debrid only\" never contacts the swarm, and your login stays in the Windows Credential Manager."), features));
    layout->addWidget(features, 0, Qt::AlignHCenter);
    layout->addStretch();
    return page;
}

QWidget *OnboardingDialog::createHybridPage()
{
    QWidget *page = makePage(new Onboarding::HybridArt, tr("One download, two ways to finish it")
            , tr("qBitFusion can fill a torrent from your debrid service and from the torrent swarm. "
                 "If the service drops out, the swarm can take over, and nothing starts over: "
                 "finished pieces stay finished, and every piece is checked against the torrent."), nullptr);
    QVBoxLayout *layout = pageLayout(page);
    layout->addSpacing(18);

    // The three ways a torrent can download, side by side.
    auto *modes = new QWidget(page);
    modes->setFixedWidth(CONTENT_WIDTH + 40);
    auto *row = new QHBoxLayout(modes);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(20);
    const struct { Hybrid::TransportPolicy policy; QString name; } entries[] = {
        {Hybrid::TransportPolicy::DebridOnly, tr("Debrid only")},
        {Hybrid::TransportPolicy::DebridPreferred, tr("Debrid preferred")},
        {Hybrid::TransportPolicy::TorrentOnly, tr("BitTorrent")}
    };
    for (const auto &entry : entries)
    {
        auto *column = new QVBoxLayout;
        column->setSpacing(3);
        auto *heading = new QHBoxLayout;
        heading->setSpacing(6);
        auto *icon = new QLabel(modes);
        icon->setPixmap(HybridGui::iconFor(entry.policy).pixmap(QSize(16, 16), screenRatio()));
        heading->addWidget(icon);
        auto *name = new QLabel(entry.name, modes);
        QFont font = name->font();
        font.setWeight(QFont::DemiBold);
        name->setFont(font);
        heading->addWidget(name);
        heading->addStretch();
        column->addLayout(heading);
        column->addWidget(wrapTo(hintLabel(policyDescription(entry.policy), modes, (Qt::AlignLeft | Qt::AlignTop)), MODE_TEXT_WIDTH));
        column->addStretch();
        row->addLayout(column, 1);
    }
    layout->addWidget(modes, 0, Qt::AlignHCenter);
    layout->addSpacing(10);
    layout->addWidget(hintLabel(tr("You choose for each torrent when you add it, and can change it later from its menu."), page));
    layout->addStretch();
    return page;
}

QWidget *OnboardingDialog::createConnectPage()
{
    m_connectArt = new Onboarding::ConnectArt;
    QWidget *page = makePage(m_connectArt, tr("Connect your debrid service")
            , tr("Link a Real-Debrid account and qBitFusion can download torrents from it. "
                 "You approve the link on Real-Debrid's website, so qBitFusion never sees your password."), nullptr);
    QVBoxLayout *layout = pageLayout(page);
    layout->addSpacing(20);

    m_connectStates = new QStackedWidget(page);
    m_connectStates->setFixedWidth(CONTENT_WIDTH);

    // Not linked yet.
    {
        auto *state = new QWidget(m_connectStates);
        auto *box = new QVBoxLayout(state);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(12);
        auto *connectButton = new QPushButton(tr("Connect Real-Debrid"), state);
        connectButton->setObjectName(u"qbfPrimaryButton"_s);
        connectButton->setCursor(Qt::PointingHandCursor);
        connectButton->setAutoDefault(false);
        connectButton->setMinimumWidth(200);
        connect(connectButton, &QPushButton::clicked, this, &OnboardingDialog::startSignIn);
        box->addWidget(connectButton, 0, Qt::AlignHCenter);
        box->addWidget(hintLabel(tr("No debrid service? Skip this step: qBitFusion then works like a normal torrent app."), state));
        box->addStretch();
        m_connectStates->addWidget(state);
    }

    // Waiting for approval on Real-Debrid's website.
    {
        auto *state = new QWidget(m_connectStates);
        auto *box = new QVBoxLayout(state);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(10);
        m_codeHint = hintLabel(tr("Requesting a sign-in code…"), state);
        box->addWidget(m_codeHint);

        m_code = new QLabel(u"········"_s, state);
        QFont codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        codeFont.setPointSizeF(font().pointSizeF() * 2.2);
        codeFont.setWeight(QFont::DemiBold);
        codeFont.setLetterSpacing(QFont::AbsoluteSpacing, 5);
        m_code->setFont(codeFont);
        m_code->setAlignment(Qt::AlignCenter);
        m_code->setTextInteractionFlags(Qt::TextSelectableByMouse);
        box->addWidget(m_code);

        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(8);
        buttons->addStretch();
        m_openPage = new QPushButton(tr("Open sign-in page"), state);
        m_openPage->setObjectName(u"qbfPrimaryButton"_s);
        m_openPage->setAutoDefault(false);
        m_openPage->setEnabled(false);
        connect(m_openPage, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(m_verificationUrl); });
        m_copyCode = new QPushButton(tr("Copy code"), state);
        m_copyCode->setAutoDefault(false);
        m_copyCode->setEnabled(false);
        connect(m_copyCode, &QPushButton::clicked, this, [this]
        {
            QApplication::clipboard()->setText(m_code->text());
            m_copyCode->setText(tr("Copied"));
            QTimer::singleShot(1500, m_copyCode, [this] { m_copyCode->setText(tr("Copy code")); });
        });
        auto *cancel = new QPushButton(tr("Cancel"), state);
        cancel->setFlat(true);
        cancel->setAutoDefault(false);
        connect(cancel, &QPushButton::clicked, this, [this]
        {
            cancelSignIn();
            refreshAccount(false);
        });
        buttons->addWidget(m_openPage);
        buttons->addWidget(m_copyCode);
        buttons->addWidget(cancel);
        buttons->addStretch();
        box->addLayout(buttons);
        m_waitStatus = hintLabel({}, state);
        box->addWidget(m_waitStatus);
        box->addStretch();
        m_connectStates->addWidget(state);
    }

    // Linked: the account, and what new torrents will do.
    {
        auto *state = new QWidget(m_connectStates);
        auto *box = new QVBoxLayout(state);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(10);
        m_accountLabel = new QLabel(state);
        m_accountLabel->setAlignment(Qt::AlignCenter);
        m_accountLabel->setTextFormat(Qt::RichText);
        box->addWidget(m_accountLabel);

        m_sourcesLabel = hintLabel({}, state);
        box->addWidget(m_sourcesLabel);
        m_turnOn = new QPushButton(tr("Turn on debrid sources"), state);
        m_turnOn->setAutoDefault(false);
        connect(m_turnOn, &QPushButton::clicked, this, []
        {
            Hybrid::HybridManager::instance()->setEnabled(true);
        });
        box->addWidget(m_turnOn, 0, Qt::AlignHCenter);

        m_defaultChoice = new QWidget(state);
        auto *choice = new QVBoxLayout(m_defaultChoice);
        choice->setContentsMargins(70, 4, 0, 0);
        choice->setSpacing(6);
        auto *choiceTitle = new QLabel(tr("New torrents start with:"), m_defaultChoice);
        QFont font = choiceTitle->font();
        font.setWeight(QFont::DemiBold);
        choiceTitle->setFont(font);
        choice->addWidget(choiceTitle);
        m_defaultGroup = new QButtonGroup(this);
        const struct { Hybrid::TransportPolicy policy; QString name; } options[] = {
            {Hybrid::TransportPolicy::DebridOnly, tr("Debrid only")},
            {Hybrid::TransportPolicy::DebridPreferred, tr("Debrid preferred")},
            {Hybrid::TransportPolicy::TorrentOnly, tr("BitTorrent")}
        };
        for (const auto &option : options)
        {
            auto *line = new QHBoxLayout;
            line->setSpacing(8);
            auto *radio = new QRadioButton(option.name, m_defaultChoice);
            radio->setIcon(HybridGui::iconFor(option.policy));
            m_defaultGroup->addButton(radio, static_cast<int>(option.policy));
            line->addWidget(radio);
            line->addWidget(hintLabel(policyDescription(option.policy), m_defaultChoice, (Qt::AlignLeft | Qt::AlignVCenter)), 1);
            choice->addLayout(line);
        }
        connect(m_defaultGroup, &QButtonGroup::idClicked, this, [](const int id)
        {
            Hybrid::HybridManager::instance()->setDefaultPolicy(static_cast<Hybrid::TransportPolicy>(id));
        });
        box->addWidget(m_defaultChoice);
        box->addStretch();
        m_connectStates->addWidget(state);
    }

    // Something went wrong.
    {
        auto *state = new QWidget(m_connectStates);
        auto *box = new QVBoxLayout(state);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(12);
        m_problemLabel = new QLabel(state);
        m_problemLabel->setAlignment(Qt::AlignCenter);
        m_problemLabel->setWordWrap(true);
        box->addWidget(m_problemLabel);
        auto *retry = new QPushButton(tr("Try again"), state);
        retry->setAutoDefault(false);
        connect(retry, &QPushButton::clicked, this, &OnboardingDialog::startSignIn);
        box->addWidget(retry, 0, Qt::AlignHCenter);
        box->addStretch();
        m_connectStates->addWidget(state);
    }

    layout->addWidget(m_connectStates, 1, Qt::AlignHCenter);
    layout->addWidget(hintLabel(tr("You can connect, switch accounts or sign out later in Options > Hybrid sources."), page));
    return page;
}

QWidget *OnboardingDialog::createLinksPage()
{
    QWidget *page = makePage(new Onboarding::LinksArt, tr("Download plain links, too")
            , tr("Copy a stream link from Stremio or a download link from your debrid service, "
                 "then choose Add > Download from URL, or press Ctrl+V in the main window. "
                 "qBitFusion checks what the link is: files download on the Links page, torrents open as usual."), nullptr);
    QVBoxLayout *layout = pageLayout(page);
    layout->addSpacing(22);

    auto *facts = new QWidget(page);
    facts->setFixedWidth(FEATURE_WIDTH);
    auto *list = new QVBoxLayout(facts);
    list->setContentsMargins(0, 0, 0, 0);
    list->setSpacing(14);
    list->addWidget(featureRow(u"view-refresh"_s, tr("Picks up where it stopped")
            , tr("Pause, lose the connection or restart: downloads continue from what is already saved."), facts));
    list->addWidget(featureRow(u"security-high"_s, tr("Links stay private")
            , tr("Links can contain your API key. Only the server's name is ever shown or logged."), facts));
    layout->addWidget(facts, 0, Qt::AlignHCenter);
    layout->addStretch();
    return page;
}

QWidget *OnboardingDialog::createDonePage()
{
    QWidget *page = makePage(new Onboarding::DoneArt, tr("You're all set")
            , tr("Here's where to find everything."), nullptr);
    QVBoxLayout *layout = pageLayout(page);
    layout->addSpacing(24);

    auto *list = new QWidget(page);
    list->setFixedWidth(FEATURE_WIDTH);
    m_doneList = new QVBoxLayout(list);
    m_doneList->setContentsMargins(0, 0, 0, 0);
    m_doneList->setSpacing(16);
    layout->addWidget(list, 0, Qt::AlignHCenter);
    layout->addStretch();
    return page;
}

void OnboardingDialog::refreshDonePage()
{
    while (QLayoutItem *item = m_doneList->takeAt(0))
    {
        delete item->widget();
        delete item;
    }

    QWidget *parent = m_doneList->parentWidget();
    const bool linked = m_provider && (m_provider->accountState() == Hybrid::AccountState::Connected);
    const bool enabled = Hybrid::HybridManager::instance() && Hybrid::HybridManager::instance()->isEnabled();
    if (linked && enabled)
    {
        const QString name = m_provider->accountName();
        m_doneList->addWidget(featureRow(u"checked-completed"_s, tr("Real-Debrid is connected")
                , (name.isEmpty() ? tr("Debrid sources are on.") : tr("Signed in as %1. Debrid sources are on.").arg(name)), parent));
    }
    else
    {
        m_doneList->addWidget(featureRow(u"network-server"_s, tr("Connect a debrid service any time")
                , tr("Options (the gear in the header) > Hybrid sources."), parent));
    }
    m_doneList->addWidget(featureRow(u"insert-link"_s, tr("Links is next to Transfers")
            , tr("Direct downloads have their own page in the header."), parent));
    m_doneList->addWidget(featureRow(u"qbf-add"_s, tr("Pick a source as you add a torrent")
            , tr("The add dialog shows where the torrent will come from."), parent));
    m_doneList->addWidget(featureRow(u"qbf-menu"_s, tr("See this tour again")
            , tr("Menu > Help > Welcome tour."), parent));
}

// ------------------------------------------------------------------------------- navigation

void OnboardingDialog::goTo(const int index)
{
    if ((index < 0) || (index >= PAGE_COUNT))
        return;

    const int previous = m_pages->currentIndex();
    if (index == PAGE_DONE)
        refreshDonePage();
    m_pages->setCurrentIndex(index);
    m_dots->setCurrent(index);
    updateButtons();

    // Slide in from the side it comes from, and fade in.
    QWidget *page = m_pages->currentWidget();
    if (previous == index)
        return;
    auto *effect = new QGraphicsOpacityEffect(page);
    effect->setOpacity(0);
    page->setGraphicsEffect(effect);
    const QPoint home = page->pos();
    const int dx = (index > previous) ? 28 : -28;

    auto *group = new QParallelAnimationGroup(page);
    auto *fade = new QPropertyAnimation(effect, "opacity", group);
    fade->setDuration(260);
    fade->setStartValue(0.0);
    fade->setEndValue(1.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);
    auto *slide = new QPropertyAnimation(page, "pos", group);
    slide->setDuration(300);
    slide->setStartValue(home + QPoint(dx, 0));
    slide->setEndValue(home);
    slide->setEasingCurve(QEasingCurve::OutCubic);
    connect(group, &QAbstractAnimation::finished, page, [page, home]
    {
        page->move(home);
        page->setGraphicsEffect(nullptr);
    });
    group->start(QAbstractAnimation::DeleteWhenStopped);
}

void OnboardingDialog::updateButtons()
{
    const int index = m_pages->currentIndex();
    const bool last = (index == (PAGE_COUNT - 1));
    m_back->setVisible(index > 0);
    m_skip->setVisible(!last);

    QString next = tr("Continue");
    if (last)
        next = tr("Start using qBitFusion");
    else if (index == m_connectPage)
        next = (m_provider && (m_provider->accountState() == Hybrid::AccountState::Connected)) ? tr("Continue") : tr("Not now");
    m_next->setText(next);
}

// ------------------------------------------------------------------------------- Real-Debrid

void OnboardingDialog::startSignIn()
{
    if (!m_provider || m_signingIn)
        return;
    m_signingIn = true;
    m_code->setText(u"········"_s);
    m_codeHint->setText(tr("Requesting a sign-in code…"));
    m_waitStatus->clear();
    m_openPage->setEnabled(false);
    m_copyCode->setEnabled(false);
    showConnectState(CONNECT_WAITING, false);
    m_provider->startDeviceAuthorization();
}

void OnboardingDialog::cancelSignIn()
{
    if (!m_signingIn)
        return;
    m_signingIn = false;
    m_countdown->stop();
    if (m_provider)
        m_provider->cancelDeviceAuthorization();
}

void OnboardingDialog::onDeviceCode(const QString &userCode, const QUrl &verificationUrl, const int expiresInSeconds)
{
    if (!m_signingIn)
        return;
    m_verificationUrl = verificationUrl;
    m_code->setText(userCode);
    m_codeHint->setText(tr("Open %1 and enter this code:").arg(verificationUrl.toDisplayString(QUrl::RemoveScheme).mid(2)));
    m_openPage->setEnabled(true);
    m_copyCode->setEnabled(true);
    m_remaining = expiresInSeconds;
    m_countdown->start(1000);
    tick();
}

void OnboardingDialog::tick()
{
    m_remaining = std::max(0, m_remaining - 1);
    m_waitStatus->setText(tr("Waiting for you to approve it… The code expires in %1:%2.")
            .arg(m_remaining / 60).arg(m_remaining % 60, 2, 10, QLatin1Char('0')));
}

void OnboardingDialog::onSignInFinished(const bool ok, const QString &message)
{
    if (!m_signingIn)
        return;
    m_signingIn = false;
    m_countdown->stop();
    if (ok)
    {
        // Connecting here means wanting to use it: debrid sources are switched on.
        Hybrid::HybridManager::instance()->setEnabled(true);
        m_checking = true;   // the provider asks for the account details right after signing in
        refreshAccount(true);
    }
    else
    {
        m_problemLabel->setText(message.isEmpty() ? tr("Signing in did not work. Please try again.") : message);
        showConnectState(CONNECT_PROBLEM, false);
    }
}

void OnboardingDialog::refreshAccount(const bool justLinked)
{
    if (m_signingIn)
        return;

    const Hybrid::AccountState state = m_provider ? m_provider->accountState() : Hybrid::AccountState::NotConfigured;
    switch (state)
    {
    case Hybrid::AccountState::Connected:
        {
            const QString success = Onboarding::toneColor(static_cast<int>(ModernAppearance::Tone::Success)).name();
            QString text = u"<span style=\"color:%1; font-weight:600\">%2</span>"_s.arg(success, tr("Connected"));
            if (!m_provider->accountName().isEmpty())
                text += u" "_s + tr("as <b>%1</b>").arg(m_provider->accountName().toHtmlEscaped());
            if (m_provider->isUsingPrivateToken())
                text += u" · "_s + tr("private token");
            else if (m_provider->premiumDaysLeft() > 0)
                text += u" · "_s + tr("Premium: %n day(s) left", nullptr, m_provider->premiumDaysLeft());
            m_accountLabel->setText(text);

            const auto *manager = Hybrid::HybridManager::instance();
            const bool enabled = manager && manager->isEnabled();
            QString check;
            if (!m_provider->accountName().isEmpty())
                check = u"<span style=\"color:%1\">%2</span>"_s.arg(success, tr("Verified: your account answered just now."));
            else if (m_checking)
                check = tr("Checking that your account answers…");
            else
                check = tr("Real-Debrid did not answer just now. qBitFusion tries again when it needs the account.");
            m_sourcesLabel->setTextFormat(Qt::RichText);
            m_sourcesLabel->setText(check + u"<br>"_s + (enabled ? tr("Debrid sources are on.")
                    : tr("Debrid sources are off, so torrents download from peers only.")));
            m_turnOn->setVisible(!enabled);
            m_defaultChoice->setVisible(enabled);
            if (enabled)
            {
                if (QAbstractButton *button = m_defaultGroup->button(static_cast<int>(manager->defaultPolicy())))
                    button->setChecked(true);
            }
            showConnectState(CONNECT_LINKED, justLinked);
        }
        break;
    case Hybrid::AccountState::AuthenticationRequired:
        m_problemLabel->setText(tr("Your Real-Debrid sign-in has expired. Connect again to renew it."));
        showConnectState(CONNECT_PROBLEM, false);
        break;
    case Hybrid::AccountState::Error:
        m_problemLabel->setText(tr("Real-Debrid could not be reached. Check your connection and try again."));
        showConnectState(CONNECT_PROBLEM, false);
        break;
    case Hybrid::AccountState::NotConfigured:
        showConnectState(CONNECT_IDLE, false);
        break;
    }
}

void OnboardingDialog::showConnectState(const ConnectState state, const bool animate)
{
    const bool changed = (m_connectStates->currentIndex() != state);
    m_connectStates->setCurrentIndex(state);
    if (changed || animate)
    {
        switch (state)
        {
        case CONNECT_IDLE:
            m_connectArt->setState(Onboarding::ConnectArt::State::NotLinked, false);
            break;
        case CONNECT_WAITING:
            m_connectArt->setState(Onboarding::ConnectArt::State::Waiting, false);
            break;
        case CONNECT_LINKED:
            m_connectArt->setState(Onboarding::ConnectArt::State::Linked, animate);
            break;
        case CONNECT_PROBLEM:
            m_connectArt->setState(Onboarding::ConnectArt::State::Problem, false);
            break;
        }
    }
    updateButtons();
}
