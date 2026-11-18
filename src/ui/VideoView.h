#pragma once

#include <QImage>
#include <QWidget>

namespace adscutter {

// Displays a decoded frame, letterboxed and aspect-ratio corrected.
class VideoView : public QWidget {
    Q_OBJECT
public:
    explicit VideoView(QWidget *parent = nullptr);

    void setFrame(const QImage &image, double displayAspect);
    void clear(const QString &message = {});

    QSize sizeHint() const override { return {768, 432}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage m_image;
    double m_aspect = 16.0 / 9.0;
    QString m_message;
};

} // namespace adscutter
