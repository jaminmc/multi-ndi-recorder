#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "NdiManager.h"
#include <QGridLayout>
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <QCloseEvent>
#include <QThread>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    m_libraryModel = new RecordingLibraryModel(this);
    ui->libraryTable->setModel(m_libraryModel);
    connect(&m_masterTimer, &QTimer::timeout, this, &MainWindow::updateMasterTimer);
    m_masterTimer.start(1000);
    connect(ui->openButton, &QPushButton::clicked, this, &MainWindow::openRecording);
    connect(ui->revealButton, &QPushButton::clicked, this, &MainWindow::revealRecording);
    
    // Set platform-specific button text
#ifdef Q_OS_WIN
    ui->revealButton->setText("Reveal in Explorer");
#elif defined(Q_OS_MACOS)
    ui->revealButton->setText("Reveal in Finder");
#else
    ui->revealButton->setText("Reveal in File Manager");
#endif
    
    // Load settings before rebuilding sources
    loadSettings();
}

MainWindow::~MainWindow()
{
    saveSettings();
    for (auto rec : m_recorders)
    {
        rec->stop();
        delete rec;
    }
    delete ui;
}

void MainWindow::rebuildSources(int count)
{
    // Save settings from existing recorders before deleting them
    QVector<SourceSettings> savedSettings;
    for (auto rec : m_recorders)
    {
        savedSettings.append(rec->settings());
    }
    
    QLayoutItem *child;
    while ((child = ui->gridLayout->takeAt(0)) != nullptr)
    {
        delete child->widget();
        delete child;
    }
    m_tiles.clear();
    m_recorders.clear();

    int columns = (count == 10) ? 4 : 3;
    for (int i = 0; i < count; ++i)
    {
        SourceRecorder *rec = new SourceRecorder(nullptr);
        
        // Restore settings if we had them for this index
        if (i < savedSettings.size())
        {
            rec->applySettings(savedSettings[i]);
        }
        
        SourceTile *tile = new SourceTile(this);
        tile->setRecorder(rec);
        connect(tile, &SourceTile::settingsRequested, this, &MainWindow::handleSettings);
        connect(rec, &SourceRecorder::recordingStarted, this, [this, rec](const QString &file) {
            RecordingEntry e;
            e.fullPath = file;
            QFileInfo info(file);
            e.filename = info.fileName();
            e.sourceLabel = rec->settings().label;
            e.timestamp = info.lastModified();
            e.size = info.size();
            m_libraryModel->addEntry(e);
        });
        connect(rec, &SourceRecorder::errorOccurred, this, [this](const QString &error) {
            QMessageBox::critical(this, "Recording Error", error);
        });
        int row = i / columns;
        int col = i % columns;
        ui->gridLayout->addWidget(tile, row, col);
        m_recorders.append(rec);
        m_tiles.append(tile);
    }
}

void MainWindow::on_sourceCountSpin_valueChanged(int value)
{
    rebuildSources(value);
    saveSettings();
}

void MainWindow::on_startAllButton_clicked()
{
    for (auto rec : m_recorders)
        rec->start();
}

void MainWindow::on_stopAllButton_clicked()
{
    for (auto rec : m_recorders)
        rec->stop();
}

void MainWindow::handleSettings(SourceRecorder *recorder)
{
    SourceSettingsDialog dlg(this);
    dlg.setSettings(recorder->settings());
    if (dlg.exec() == QDialog::Accepted)
    {
        recorder->applySettings(dlg.settings());
        saveSettings();
    }
}

void MainWindow::updateMasterTimer()
{
    int total = 0;
    for (auto rec : m_recorders)
    {
        if (rec->status() == "Recording")
            ++total;
    }
    ui->masterStatusLabel->setText(QString("Active sources: %1").arg(total));
}

void MainWindow::openRecording()
{
    QModelIndex idx = ui->libraryTable->currentIndex();
    if (!idx.isValid())
        return;
    // Column 2 is the full path column (0=Source, 1=Filename, 2=Path, 3=Date, 4=Size)
    int pathColumn = 2;
    if (pathColumn >= m_libraryModel->columnCount())
        return;
    QModelIndex pathIdx = m_libraryModel->index(idx.row(), pathColumn);
    if (!pathIdx.isValid())
        return;
    QString path = m_libraryModel->data(pathIdx, Qt::DisplayRole).toString();
    if (path.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::revealRecording()
{
    QModelIndex idx = ui->libraryTable->currentIndex();
    if (!idx.isValid())
        return;
    // Column 2 is the full path column (0=Source, 1=Filename, 2=Path, 3=Date, 4=Size)
    int pathColumn = 2;
    if (pathColumn >= m_libraryModel->columnCount())
        return;
    QModelIndex pathIdx = m_libraryModel->index(idx.row(), pathColumn);
    if (!pathIdx.isValid())
        return;
    QString path = m_libraryModel->data(pathIdx, Qt::DisplayRole).toString();
    if (path.isEmpty())
        return;
#ifdef Q_OS_WIN
    QString cmd = QString("explorer.exe /select,\"%1\"").arg(path);
    system(cmd.toUtf8().constData());
#elif defined(Q_OS_MACOS)
    QString cmd = QString("open -R \"%1\"").arg(path);
    system(cmd.toUtf8().constData());
#else
    // Linux: use xdg-open or file manager
    QString cmd = QString("xdg-open \"%1\"").arg(QFileInfo(path).path());
    system(cmd.toUtf8().constData());
#endif
}

void MainWindow::loadSettings()
{
    QSettings settings;
    
    // Refresh NDI sources before loading settings to ensure we have the latest list
    NdiManager ndi;
    ndi.refreshSources();
    // Give NDI a moment to discover sources
    QThread::msleep(500);
    
    // Load source count
    int sourceCount = settings.value("sourceCount", 1).toInt();
    if (sourceCount < 1) sourceCount = 1;
    
    // Block signals to prevent double rebuild and premature save
    ui->sourceCountSpin->blockSignals(true);
    ui->sourceCountSpin->setValue(sourceCount);
    ui->sourceCountSpin->blockSignals(false);
    
    // Rebuild sources with the saved count
    rebuildSources(sourceCount);
    
    // Load settings for each recorder
    for (int i = 0; i < m_recorders.size(); ++i)
    {
        QString prefix = QString("source%1/").arg(i);
        SourceSettings s;
        s.ndiSource = settings.value(prefix + "ndiSource", "").toString();
        s.label = settings.value(prefix + "label", "").toString();
        s.outputFolder = settings.value(prefix + "outputFolder", "").toString();
        s.segmented = settings.value(prefix + "segmented", false).toBool();
        s.segmentMinutes = settings.value(prefix + "segmentMinutes", 20).toInt();
        
        // Load video encoding settings
        s.fileExtension = settings.value(prefix + "fileExtension", "mp4").toString();
        s.videoCodec = settings.value(prefix + "videoCodec", "h264").toString();
        s.useHardwareEncoder = settings.value(prefix + "useHardwareEncoder", false).toBool();
        s.qualityMode = settings.value(prefix + "qualityMode", "crf").toString();
        s.crfValue = settings.value(prefix + "crfValue", 23).toInt();
        s.videoBitrate = settings.value(prefix + "videoBitrate", 12000000).toInt();
        s.hardwareQuality = settings.value(prefix + "hardwareQuality", 55).toInt();
        
        if (!s.ndiSource.isEmpty() || !s.label.isEmpty() || !s.outputFolder.isEmpty())
        {
            m_recorders[i]->applySettings(s);
        }
    }
}

void MainWindow::saveSettings()
{
    QSettings settings;
    
    // Save source count
    settings.setValue("sourceCount", m_recorders.size());
    
    // Save settings for each recorder
    for (int i = 0; i < m_recorders.size(); ++i)
    {
        QString prefix = QString("source%1/").arg(i);
        SourceSettings s = m_recorders[i]->settings();
        settings.setValue(prefix + "ndiSource", s.ndiSource);
        settings.setValue(prefix + "label", s.label);
        settings.setValue(prefix + "outputFolder", s.outputFolder);
        settings.setValue(prefix + "segmented", s.segmented);
        settings.setValue(prefix + "segmentMinutes", s.segmentMinutes);
        
        // Save video encoding settings
        settings.setValue(prefix + "fileExtension", s.fileExtension);
        settings.setValue(prefix + "videoCodec", s.videoCodec);
        settings.setValue(prefix + "useHardwareEncoder", s.useHardwareEncoder);
        settings.setValue(prefix + "qualityMode", s.qualityMode);
        settings.setValue(prefix + "crfValue", s.crfValue);
        settings.setValue(prefix + "videoBitrate", s.videoBitrate);
        settings.setValue(prefix + "hardwareQuality", s.hardwareQuality);
    }
    
    settings.sync();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    QMainWindow::closeEvent(event);
}
