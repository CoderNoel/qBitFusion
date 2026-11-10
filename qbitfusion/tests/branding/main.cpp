#include <QApplication>
#include <QDebug>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

namespace
{
    bool visible(const QImage &image)
    {
        int opaque = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                opaque += image.pixelColor(x, y).alpha() > 128;
        return !image.isNull() && (opaque > image.width() * image.height() / 20)
            && (image.pixelColor(0, 0).alpha() == 0);
    }
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    const QList<int> sizes {16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 128, 256};
    const QStringList resources {
        QStringLiteral(":/qbitfusion/brand/qbitfusion.png"),
        QStringLiteral(":/qbitfusion/brand/tray-light.png"),
        QStringLiteral(":/qbitfusion/brand/tray-dark.png")
    };
    for (const auto &path : resources)
    {
        const QIcon icon(path);
        if (icon.isNull())
            return qCritical() << "Missing icon" << path, 1;
        for (const int size : sizes)
            for (const qreal scale : {1.0, 1.25, 1.5, 2.0, 3.0, 4.0})
                if (!visible(icon.pixmap(QSize(size, size), scale).toImage()))
                    return qCritical() << "Invisible icon" << path << size << scale, 2;
    }
    const QImage brand(resources.front());
    const QColor center = brand.pixelColor(brand.width() / 2, brand.height() / 2);
    if ((center.red() <= center.green()) || (center.green() <= center.blue()))
        return qCritical() << "Expected orange badge", 3;
#ifdef Q_OS_WIN
    for (int i = 1; i < argc; ++i)
    {
        const QString path = QString::fromLocal8Bit(argv[i]);
        if (path.endsWith(QStringLiteral(".lnk"), Qt::CaseInsensitive))
        {
            for (const UINT flags : {SHGFI_ICON | SHGFI_SMALLICON, SHGFI_ICON | SHGFI_LARGEICON})
            {
                SHFILEINFOW info {};
                if (!SHGetFileInfoW(reinterpret_cast<LPCWSTR>(path.utf16()), 0, &info, sizeof(info), flags))
                    return qCritical() << "Windows Shell cannot resolve shortcut" << path, 7;
                const QImage image = QImage::fromHICON(info.hIcon);
                DestroyIcon(info.hIcon);
                if (!visible(image))
                    return qCritical() << "Windows Shell resolved an invisible shortcut icon" << path, 8;
                const QColor badge = image.pixelColor(image.width() / 2, image.height() / 4);
                if ((badge.red() <= badge.green()) || (badge.green() <= badge.blue()))
                    return qCritical() << "Windows Shell resolved an outdated shortcut icon" << path, 9;
            }
            qInfo().noquote() << "Windows Shell shortcut resolution passed:" << path;
            continue;
        }
        for (const int size : sizes)
        {
            HICON handle = nullptr;
            const UINT count = PrivateExtractIconsW(reinterpret_cast<LPCWSTR>(path.utf16()),
                0, size, size, &handle, nullptr, 1, 0);
            if ((count != 1) || !handle)
                return qCritical() << "Windows cannot extract icon" << path << size, 4;
            const QImage image = QImage::fromHICON(handle);
            DestroyIcon(handle);
            if ((image.size() != QSize(size, size)) || !visible(image))
                return qCritical() << "Windows extracted invisible/wrong-size icon" << path << size, 5;
            const QColor badge = image.pixelColor(size / 2, size / 4);
            if ((badge.red() <= badge.green()) || (badge.green() <= badge.blue()))
                return qCritical() << "Windows extracted an outdated non-orange icon" << path << size, 6;
        }
        qInfo().noquote() << "Windows extraction passed:" << path;
    }
#endif
    qInfo() << "PASS: 3 Qt resources x 15 sizes x 6 display scales; transparent corners and visible orange badge";
}
