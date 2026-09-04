// src/ui/common/TitleBar.cpp —— 切片 45 自绘标题栏实现。
#include "common/TitleBar.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpacerItem>

namespace ens::ui {

TitleBar::TitleBar(QWidget* parent)
    : QWidget(parent) {
    setupUi();
    setFixedHeight(preferredHeight());
    // objectName 便于 QSS 选择器 #EnerSentryTitleBar 命中（resources/qss/theme.qss）
    setObjectName(QStringLiteral("EnerSentryTitleBar"));
    // 关闭按钮特殊选择器（hover 红底）
    if (m_closeBtn) m_closeBtn->setObjectName(QStringLiteral("closeBtn"));
    if (m_maxBtn)   m_maxBtn->setObjectName(QStringLiteral("maxBtn"));
    if (m_minBtn)   m_minBtn->setObjectName(QStringLiteral("minBtn"));
}

TitleBar::~TitleBar() = default;

void TitleBar::setupUi() {
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(8, 0, 0, 0);
    root->setSpacing(0);

    m_logo = new QLabel(this);
    m_logo->setFixedSize(20, 20);
    m_logo->setScaledContents(true);
    m_logo->setPixmap(QIcon(m_titleIconPath).pixmap(20, 20));
    root->addWidget(m_logo);

    root->addSpacing(8);

    m_title = new QLabel(this);
    m_title->setText(QStringLiteral("EnerSentry"));
    m_title->setObjectName(QStringLiteral("titleLabel"));
    root->addWidget(m_title);

    auto* spacer = new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum);
    root->addItem(spacer);

    m_minBtn = new QPushButton(this);
    m_minBtn->setIcon(QIcon(QStringLiteral(":/icons/window_minimize.svg")));
    m_minBtn->setToolTip(QStringLiteral("最小化"));
    m_minBtn->setFixedSize(46, 32);
    root->addWidget(m_minBtn);

    m_maxBtn = new QPushButton(this);
    m_maxBtn->setIcon(QIcon(QStringLiteral(":/icons/window_maximize.svg")));
    m_maxBtn->setToolTip(QStringLiteral("最大化"));
    m_maxBtn->setFixedSize(46, 32);
    root->addWidget(m_maxBtn);

    m_closeBtn = new QPushButton(this);
    m_closeBtn->setIcon(QIcon(QStringLiteral(":/icons/window_close.svg")));
    m_closeBtn->setToolTip(QStringLiteral("关闭"));
    m_closeBtn->setFixedSize(46, 32);
    root->addWidget(m_closeBtn);

    // 信号连接
    connect(m_minBtn,   &QPushButton::clicked, this, &TitleBar::minimizeClicked);
    connect(m_maxBtn,   &QPushButton::clicked, this, [this] {
        m_maxChecked = !m_maxChecked;
        updateMaximizeIcon();
        emit maximizeToggled(m_maxChecked);
    });
    connect(m_closeBtn, &QPushButton::clicked, this, &TitleBar::closeClicked);
}

void TitleBar::setTitle(const QString& title) {
    if (m_title) m_title->setText(title);
}

QString TitleBar::title() const {
    return m_title ? m_title->text() : QString();
}

void TitleBar::setTitleIconPath(const QString& qrcPath) {
    m_titleIconPath = qrcPath;
    if (m_logo) m_logo->setPixmap(QIcon(qrcPath).pixmap(20, 20));
}

void TitleBar::setMinimumVisible(bool v) {
    if (m_minBtn) m_minBtn->setVisible(v);
}

void TitleBar::setMaximumVisible(bool v) {
    if (m_maxBtn) m_maxBtn->setVisible(v);
}

void TitleBar::setMaximizeButtonChecked(bool checked) {
    m_maxChecked = checked;
    updateMaximizeIcon();
}

void TitleBar::updateMaximizeIcon() {
    if (!m_maxBtn) return;
    m_maxBtn->setIcon(QIcon(m_maxChecked
        ? QStringLiteral(":/icons/window_restore.svg")
        : QStringLiteral(":/icons/window_maximize.svg")));
    m_maxBtn->setToolTip(m_maxChecked
        ? QStringLiteral("还原")
        : QStringLiteral("最大化"));
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_dragging = false;   // 双击不进入拖拽
        m_maxChecked = !m_maxChecked;
        updateMaximizeIcon();
        emit doubleClicked();
        emit maximizeToggled(m_maxChecked);
        e->accept();
    } else {
        QWidget::mouseDoubleClickEvent(e);
    }
}

void TitleBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        m_pressGlobal = e->globalPos();
        emit dragPressed(m_pressGlobal);
        e->accept();
    } else {
        QWidget::mousePressEvent(e);
    }
}

void TitleBar::mouseMoveEvent(QMouseEvent* e) {
    if (m_dragging && (e->buttons() & Qt::LeftButton)) {
        // 不在此处 move window：交给 FramelessHelper（其知道是否可移动/缩放）
        e->accept();
    } else {
        QWidget::mouseMoveEvent(e);
    }
}

void TitleBar::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_dragging = false;
        e->accept();
    } else {
        QWidget::mouseReleaseEvent(e);
    }
}

}  // namespace ens::ui
