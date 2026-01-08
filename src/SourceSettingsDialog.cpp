#include "SourceSettingsDialog.h"
#include "ui_SourceSettingsDialog.h"
#include <QFileDialog>

SourceSettingsDialog::SourceSettingsDialog(QWidget *parent)
    : QDialog(parent), ui(new Ui::SourceSettingsDialog)
{
    ui->setupUi(this);
    refreshNdi();
    connect(ui->refreshNdiButton, &QPushButton::clicked, this, &SourceSettingsDialog::refreshNdi);
    connect(ui->chooseFolderButton, &QPushButton::clicked, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this, tr("Output Folder"), ui->folderEdit->text());
        if (!dir.isEmpty())
            ui->folderEdit->setText(dir);
    });
    
    // Create explicit button groups to ensure radio buttons don't interfere with each other
    m_recordingModeGroup = new QButtonGroup(this);
    m_recordingModeGroup->addButton(ui->modeContinuous, 0);
    m_recordingModeGroup->addButton(ui->modeSegmented, 1);
    
    m_qualityModeGroup = new QButtonGroup(this);
    m_qualityModeGroup->addButton(ui->qualityModeCRF, 0);
    m_qualityModeGroup->addButton(ui->qualityModeBitrate, 1);
    
    // Initialize file extension combo
    ui->fileExtensionCombo->addItems({"mp4", "mov", "mkv"});
    
    // Initialize video codec combo with available encoders
    QVector<QString> availableEncoders = FfmpegWriter::getAvailableEncoders();
    QStringList encoderList;
    for (const QString &encoder : availableEncoders)
    {
        encoderList.append(encoder);
    }
    // Sort encoders for better UX (software encoders first, then hardware)
    encoderList.sort();
    ui->videoCodecCombo->addItems(encoderList);
    
    // Enable locale-aware group separator for bitrate spinbox
    ui->bitrateSpin->setGroupSeparatorShown(true);
    
    // Connect signals to update UI state
    connect(ui->fileExtensionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SourceSettingsDialog::updateUIState);
    connect(ui->videoCodecCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SourceSettingsDialog::updateUIState);
    connect(ui->qualityModeCRF, &QRadioButton::toggled, this, &SourceSettingsDialog::updateUIState);
    
    // Explicitly connect button box signals
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &SourceSettingsDialog::on_buttonBox_rejected);
    
    updateUIState();
}

SourceSettingsDialog::~SourceSettingsDialog()
{
    // Button groups are parented to this dialog, so Qt will delete them automatically
    delete ui;
}

void SourceSettingsDialog::setSettings(const SourceSettings &settings)
{
    // Store the settings first
    m_settings = settings;
    
    // Block all signals while setting values to prevent any interference
    ui->modeSegmented->blockSignals(true);
    ui->modeContinuous->blockSignals(true);
    ui->qualityModeCRF->blockSignals(true);
    ui->qualityModeBitrate->blockSignals(true);
    ui->fileExtensionCombo->blockSignals(true);
    ui->videoCodecCombo->blockSignals(true);
    
    // Try to set the saved NDI source
    QString savedNdiSource = settings.ndiSource;
    if (!savedNdiSource.isEmpty())
    {
        int index = ui->ndiCombo->findText(savedNdiSource);
        if (index >= 0)
        {
            ui->ndiCombo->setCurrentIndex(index);
        }
        else
        {
            // Source not currently available, but preserve it in the combo
            ui->ndiCombo->insertItem(0, savedNdiSource);
            ui->ndiCombo->setCurrentIndex(0);
        }
    }
    else if (ui->ndiCombo->count() > 0)
    {
        ui->ndiCombo->setCurrentIndex(0);
    }
    
    ui->labelEdit->setText(settings.label);
    ui->folderEdit->setText(settings.outputFolder);
    ui->segmentSpin->setValue(settings.segmentMinutes);
    
    // Set mode radio buttons
    ui->modeSegmented->setChecked(settings.segmented);
    ui->modeContinuous->setChecked(!settings.segmented);
    
    // Set video encoding settings - ensure defaults if not set
    QString fileExt = settings.fileExtension.isEmpty() ? "mp4" : settings.fileExtension;
    int extIndex = ui->fileExtensionCombo->findText(fileExt);
    if (extIndex >= 0)
        ui->fileExtensionCombo->setCurrentIndex(extIndex);
    else
        ui->fileExtensionCombo->setCurrentIndex(0);
    
    QString codec = settings.videoCodec.isEmpty() ? "libx264" : settings.videoCodec;
    // If old generic names were saved, try to find a matching encoder
    if (codec == "h264")
        codec = "libx264";
    else if (codec == "h265" || codec == "hevc")
        codec = "libx265";
    else if (codec == "copy")
        codec = "libx264";
    
    int codecIndex = ui->videoCodecCombo->findText(codec);
    if (codecIndex >= 0)
        ui->videoCodecCombo->setCurrentIndex(codecIndex);
    else
        ui->videoCodecCombo->setCurrentIndex(0);
    
    QString qualityMode = settings.qualityMode.isEmpty() ? "crf" : settings.qualityMode;
    if (qualityMode == "crf")
        ui->qualityModeCRF->setChecked(true);
    else
        ui->qualityModeBitrate->setChecked(true);
    
    // Set CRF/CQ value - will be adjusted by updateUIState based on encoder type
    ui->crfSpin->setValue(settings.crfValue);
    // Convert bps to kbps for display (divide by 1000)
    ui->bitrateSpin->setValue(settings.videoBitrate / 1000);
    
    // Unblock all signals
    ui->modeSegmented->blockSignals(false);
    ui->modeContinuous->blockSignals(false);
    ui->qualityModeCRF->blockSignals(false);
    ui->qualityModeBitrate->blockSignals(false);
    ui->fileExtensionCombo->blockSignals(false);
    ui->videoCodecCombo->blockSignals(false);
    
    // Update UI state after all values are set
    updateUIState();
}

SourceSettings SourceSettingsDialog::settings() const
{
    SourceSettings s;
    s.ndiSource = ui->ndiCombo->currentText();
    s.outputFolder = ui->folderEdit->text();
    s.label = ui->labelEdit->text();
    s.segmentMinutes = ui->segmentSpin->value();
    s.segmented = ui->modeSegmented->isChecked();
    
    // Video encoding settings
    s.fileExtension = ui->fileExtensionCombo->currentText();
    s.videoCodec = ui->videoCodecCombo->currentText();
    // Hardware encoder is now detected from codec name, so we don't need this field
    // But keep it for backward compatibility - detect from codec name
    QString codecName = s.videoCodec.toLower();
    s.useHardwareEncoder = codecName.contains("_nvenc") ||
                          codecName.contains("_amf") ||
                          codecName.contains("_qsv") ||
                          codecName.contains("_vaapi") ||
                          codecName.contains("_videotoolbox") ||
                          codecName.contains("_mf") ||
                          codecName.contains("_mediacodec");
    s.qualityMode = ui->qualityModeCRF->isChecked() ? "crf" : "bitrate";
    // For VideoToolbox, crfValue stores CQ (1-100), for others it stores CRF (0-51)
    s.crfValue = ui->crfSpin->value();
    // Convert kbps to bps for storage (multiply by 1000)
    s.videoBitrate = ui->bitrateSpin->value() * 1000;
    // hardwareQuality is deprecated but kept for backward compatibility
    // For VideoToolbox, use crfValue (which contains CQ), otherwise use default
    s.hardwareQuality = (codecName.contains("_videotoolbox") && s.crfValue > 51) ? s.crfValue : 55;
    
    return s;
}

void SourceSettingsDialog::refreshNdi()
{
    // Preserve the current selection
    QString currentSelection = ui->ndiCombo->currentText();
    
    ui->ndiCombo->clear();
    ui->ndiCombo->addItems(m_ndi.availableSources());
    
    // Try to restore the previous selection
    if (!currentSelection.isEmpty())
    {
        int index = ui->ndiCombo->findText(currentSelection);
        if (index >= 0)
        {
            ui->ndiCombo->setCurrentIndex(index);
        }
        else
        {
            // Previous selection not in new list, but preserve it
            ui->ndiCombo->insertItem(0, currentSelection);
            ui->ndiCombo->setCurrentIndex(0);
        }
    }
}

void SourceSettingsDialog::on_buttonBox_accepted()
{
    m_settings = settings();
    accept();
}

void SourceSettingsDialog::on_buttonBox_rejected()
{
    reject();
}

void SourceSettingsDialog::updateUIState()
{
    QString codec = ui->videoCodecCombo->currentText().toLower();
    
    // Detect if selected codec is a hardware encoder and which type
    bool isVideoToolbox = codec.contains("_videotoolbox");
    
    // Update quality mode radio button text: "CRF" for most encoders, "CQ" for VideoToolbox
    if (isVideoToolbox)
    {
        ui->qualityModeCRF->setText("CQ");
    }
    else
    {
        ui->qualityModeCRF->setText("CRF");
    }
    
    // Quality controls are always enabled
    ui->qualityModeCRF->setEnabled(true);
    ui->qualityModeBitrate->setEnabled(true);
    
    // Update CRF/CQ spinbox and label based on encoder type
    if (isVideoToolbox)
    {
        // VideoToolbox: CQ mode (1-100, higher is better)
        ui->crfSpin->setMinimum(1);
        ui->crfSpin->setMaximum(100);
        // Set default to 55 if currently at CRF default (23)
        if (ui->crfSpin->value() <= 51)
        {
            ui->crfSpin->setValue(55);
        }
        ui->label_10->setText("CQ Value: (1-100 Higher is Better)");
        ui->crfSpin->setEnabled(ui->qualityModeCRF->isChecked());
    }
    else
    {
        // Other encoders: CRF mode (1-51, lower is better)
        ui->crfSpin->setMinimum(1);
        ui->crfSpin->setMaximum(51);
        // Set default to 23 if currently at CQ value (>51)
        if (ui->crfSpin->value() > 51)
        {
            ui->crfSpin->setValue(23);
        }
        ui->label_10->setText("CRF Value: (1-51 Lower is Better)");
        ui->crfSpin->setEnabled(ui->qualityModeCRF->isChecked());
    }
    
    ui->bitrateSpin->setEnabled(ui->qualityModeBitrate->isChecked());
}
