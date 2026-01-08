#pragma once
#include <QMainWindow>
#include <QVector>
#include <QTimer>
#include <QCloseEvent>
#include "SourceRecorder.h"
#include "SourceTile.h"
#include "SourceSettingsDialog.h"
#include "RecordingLibraryModel.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void on_sourceCountSpin_valueChanged(int value);
    void on_startAllButton_clicked();
    void on_stopAllButton_clicked();
    void handleSettings(SourceRecorder *recorder);
    void updateMasterTimer();
    void openRecording();
    void revealRecording();

private:
    void rebuildSources(int count);
    void loadSettings();
    void saveSettings();
    void closeEvent(QCloseEvent *event) override;

    Ui::MainWindow *ui;
    QVector<SourceRecorder *> m_recorders;
    QVector<SourceTile *> m_tiles;
    QTimer m_masterTimer;
    RecordingLibraryModel *m_libraryModel;
};
