#include "labeledtoolbutton.h"

#include <QPainter>

LabeledToolButton::LabeledToolButton(QWidget *parent) : QToolButton(parent)
{

}

void LabeledToolButton::paintEvent(QPaintEvent *event)
{
    QToolButton::paintEvent(event);

    drawBackground();

    QPainter p(this);

    QFont textFont = p.font();
    textFont.setPixelSize(9);
    p.setFont(textFont);
    const int textHeight = p.fontMetrics().height();
    QColor textBgColor(200, 200, 200, 240);

    // draw labels
    //
    QString nameText = toolName();
    if (nameText.length() > 0) {
        int textWidth = p.fontMetrics().boundingRect(nameText).width();
        QPoint textP(2, textHeight + 2);
        p.fillRect(0, 0, textWidth + 4, textHeight + 4, textBgColor);
        p.setPen(Qt::black);
        p.drawText(textP, nameText);
    }

    QString valueText = toolValue();
    if (valueText.length() > 0) {
        int textWidth = p.fontMetrics().boundingRect(valueText).width();
        QPoint textP(width() - textWidth - 2, height() - 2);
        p.fillRect(textP.x() - 2, textP.y() - 2 - textHeight, textWidth + 4, textHeight + 4, textBgColor);
        p.setPen(Qt::black);
        p.drawText(textP, valueText);
    }
}
