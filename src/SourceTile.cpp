#include "SourceTile.h"
#include "ui_SourceTile.h"
#include "NdiManager.h"
#include <QDateTime>
#include <QPixmap>
#include <QThread>

SourceTile::SourceTile(QWidget *parent)
    : QWidget(parent), ui(new Ui::SourceTile), m_recorder(nullptr)
{
    ui->setupUi(this);
    connect(&m_timer, &QTimer::timeout, this, &SourceTile::updatePreview);
    m_timer.start(500);
}

SourceTile::~SourceTile()
{
    if (m_recorder && m_recorder->status() == "Previewing")
        m_recorder->stop();
    delete ui;
}

void SourceTile::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // Update preview when widget is resized to maintain aspect ratio
    if (m_recorder)
        updatePreview();
}

void SourceTile::setRecorder(SourceRecorder *recorder)
{
    // Stop preview on old recorder if it exists
    if (m_recorder)
    {
        disconnect(m_recorder, nullptr, this, nullptr);
        if (m_recorder->status() == "Previewing")
            m_recorder->stop();
    }

    m_recorder = recorder;
    if (recorder)
    {
        // Use QueuedConnection since SourceRecorder may be in a different thread
        connect(recorder, &SourceRecorder::previewUpdated, this, &SourceTile::updatePreview, Qt::QueuedConnection);
        connect(recorder, &SourceRecorder::settingsChanged, this, &SourceTile::updateLabel);
        updateLabel();
        updatePreview();
        // Start preview if NDI source is configured
        if (!recorder->settings().ndiSource.isEmpty())
        {
            // Refresh NDI sources before starting preview
            NdiManager ndi;
            ndi.refreshSources();
            QThread::msleep(200);
            recorder->startPreview();
        }
    }
    else
    {
        ui->sourceLabel->setText("No source");
        ui->previewLabel->clear();
        ui->previewLabel->setText("Preview");
        ui->statusLabel->setText("Idle");
        ui->timerLabel->setText("00:00");
    }
}

void SourceTile::updateLabel()
{
    if (m_recorder)
    {
        QString label = m_recorder->settings().label;
        if (label.isEmpty())
            label = m_recorder->settings().ndiSource;
        if (label.isEmpty())
            label = "Unnamed Source";
        ui->sourceLabel->setText(label);
    }
    else
    {
        ui->sourceLabel->setText("No source");
    }
}

void SourceTile::updatePreview()
{
    if (!m_recorder)
        return;
    QImage frame = m_recorder->lastFrame();
    if (!frame.isNull())
    {
        // Calculate scaled size maintaining aspect ratio
        QSize labelSize = ui->previewLabel->size();
        QSize scaledSize = frame.size().scaled(labelSize, Qt::KeepAspectRatio);
        
        // Create pixmap and scale it with smooth transformation
        QPixmap pixmap = QPixmap::fromImage(frame);
        pixmap = pixmap.scaled(scaledSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        
        ui->previewLabel->setPixmap(pixmap);
        ui->previewLabel->setText(QString());
    }
    else
    {
        ui->previewLabel->clear();
        ui->previewLabel->setText("No preview");
    }
    ui->statusLabel->setText(m_recorder->status());
    int secs = m_recorder->elapsedMs() / 1000;
    ui->timerLabel->setText(QString("%1:%2").arg(secs / 60, 2, 10, QChar('0')).arg(secs % 60, 2, 10, QChar('0')));
}

void SourceTile::on_startButton_clicked()
{
    if (m_recorder)
        m_recorder->start();
}

void SourceTile::on_stopButton_clicked()
{
    if (m_recorder)
        m_recorder->stop();
}

void SourceTile::on_settingsButton_clicked()
{
    emit settingsRequested(m_recorder);
}
