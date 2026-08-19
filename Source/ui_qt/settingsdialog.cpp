#include "settingsdialog.h"
#include <cassert>
#include <cmath>
#include <iterator>
#include <QFileDialog>
#include <QMessageBox>
#include <QSignalBlocker>
#include "ui_settingsdialog.h"
#include "PS2VM_Preferences.h"
#include "PreferenceDefs.h"
#include "../gs/GSH_OpenGL/GSH_OpenGL.h"
#include "QStringUtils.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef HAS_GSH_VULKAN
#include "gs/GSH_Vulkan/GSH_VulkanDeviceInfo.h"
Q_DECLARE_METATYPE(GSH_Vulkan::VULKAN_DEVICE);
#endif

namespace
{
	void PopulateSerialPorts(QComboBox* comboBox)
	{
#ifdef _WIN32
		wchar_t devicePath[1024] = {};
		for(unsigned int portNumber = 1; portNumber <= 256; portNumber++)
		{
			auto portName = QString("COM%1").arg(portNumber);
			auto portNameWide = portName.toStdWString();
			if(QueryDosDeviceW(portNameWide.c_str(), devicePath, std::size(devicePath)) != 0)
			{
				comboBox->addItem(portName);
			}
		}
#endif
	}
}

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::SettingsDialog)
{
	ui->setupUi(this);

	//Not needed, as it can be set in the ui editor, but left for ease of ui edit.
	ui->stackedWidget->setCurrentIndex(0);

	ui->lineEdit_arcadeIOServerPort->setValidator(new QIntValidator(0, 65535, this));
	ui->comboBox_cardReaderCom->setEditable(true);
	{
		QSignalBlocker blocker(ui->comboBox_cardReaderCom);
		PopulateSerialPorts(ui->comboBox_cardReaderCom);
	}

	// this assert is to ensure no one adds an item to the combobox through qt creator by accident
	assert(ui->comboBox_gs_selection->count() == 0);
	assert(ui->comboBox_vulkan_device->count() == 0);

	ui->comboBox_gs_selection->blockSignals(true);
	ui->comboBox_vulkan_device->blockSignals(true);

	ui->comboBox_gs_selection->insertItem(SettingsDialog::GS_HANDLERS::OPENGL, "OpenGL");
#ifdef HAS_GSH_VULKAN
	auto devices = GSH_Vulkan::CDeviceInfo::GetInstance().GetAvailableDevices();
	if(!devices.empty())
	{
		ui->comboBox_gs_selection->insertItem(SettingsDialog::GS_HANDLERS::VULKAN, "Vulkan");
		auto selectedDevice = GSH_Vulkan::CDeviceInfo::GetInstance().GetSelectedDevice();
		for(const auto& device : devices)
		{
			ui->comboBox_vulkan_device->addItem(QString::fromUtf8(device.deviceName.c_str()), QVariant::fromValue(device));
			if(
			    (selectedDevice.deviceId == device.deviceId) &&
			    (selectedDevice.vendorId == device.vendorId))
			{
				auto selectedIndex = ui->comboBox_vulkan_device->count() - 1;
				ui->comboBox_vulkan_device->setCurrentIndex(selectedIndex);
			}
		}
	}
#else
	ui->button_vulkanDeviceInfo->hide();
#endif
	if(ui->comboBox_gs_selection->count() <= 1)
	{
		ui->gs_option_widget->hide();
	}

	ui->comboBox_vulkan_device->blockSignals(false);
	ui->comboBox_gs_selection->blockSignals(false);

	LoadPreferences();
	connect(ui->listWidget, &QListWidget::currentItemChanged, this, &SettingsDialog::changePage);
}

SettingsDialog::~SettingsDialog()
{
	CAppConfig::GetInstance().Save();
	delete ui;
}

void SettingsDialog::changePage(QListWidgetItem* current, QListWidgetItem* previous)
{
	if(!current)
		current = previous;

	ui->stackedWidget->setCurrentIndex(ui->listWidget->row(current));
}

void SettingsDialog::LoadPreferences()
{
	ui->comboBox_system_language->setCurrentIndex(CAppConfig::GetInstance().GetPreferenceInteger(PREF_SYSTEM_LANGUAGE));
	ui->checkBox_limitFrameRate->setChecked(CAppConfig::GetInstance().GetPreferenceBoolean(PREF_PS2_LIMIT_FRAMERATE));
	ui->checkBox_showEECPUUsage->setChecked(CAppConfig::GetInstance().GetPreferenceBoolean(PREF_UI_SHOWEECPUUSAGE));
	ui->edit_arcadeRoms_dir->setText(PathToQString(CAppConfig::GetInstance().GetPreferencePath(PREF_PS2_ARCADEROMS_DIRECTORY)));
	ui->checkBox_enableArcadeIOServer->setChecked(CAppConfig::GetInstance().GetPreferenceBoolean(PREF_PS2_ARCADE_IO_SERVER_ENABLED));
	ui->lineEdit_arcadeIOServerPort->setText(QString::number(CAppConfig::GetInstance().GetPreferenceInteger(PREF_PS2_ARCADE_IO_SERVER_PORT)));
	auto useRealCardReader = CAppConfig::GetInstance().GetPreferenceBoolean(PREF_PS2_ARCADE_USE_REAL_CARD_READER);
	auto cardReaderComPort = QString::fromStdString(CAppConfig::GetInstance().GetPreferenceString(PREF_PS2_ARCADE_CARD_READER_COM_PORT));
	{
		QSignalBlocker blocker(ui->comboBox_cardReaderCom);
		if(!cardReaderComPort.isEmpty() && (ui->comboBox_cardReaderCom->findText(cardReaderComPort, Qt::MatchFixedString) == -1))
		{
			ui->comboBox_cardReaderCom->addItem(cardReaderComPort);
		}
		ui->comboBox_cardReaderCom->setCurrentText(cardReaderComPort);
	}
	ui->checkBox_useRealCardReader->setChecked(useRealCardReader);
	ui->label_cardReaderCom->setEnabled(useRealCardReader);
	ui->comboBox_cardReaderCom->setEnabled(useRealCardReader);

	int factor = CAppConfig::GetInstance().GetPreferenceInteger(PREF_CGSH_OPENGL_RESOLUTION_FACTOR);
	int factor_index = std::log2(factor);
	ui->comboBox_res_multiplyer->setCurrentIndex(factor_index);
	ui->checkBox_widescreenOutput->setChecked(CAppConfig::GetInstance().GetPreferenceBoolean(PREF_CGSHANDLER_WIDESCREEN));
	ui->checkBox_enable_gs_ram_reads->setChecked(CAppConfig::GetInstance().GetPreferenceBoolean(PREF_CGSHANDLER_GS_RAM_READS_ENABLED));
	ui->checkBox_force_bilinear_filtering->setChecked(CAppConfig::GetInstance().GetPreferenceBoolean(PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES));
	ui->comboBox_gs_selection->setCurrentIndex(CAppConfig::GetInstance().GetPreferenceInteger(PREF_VIDEO_GS_HANDLER));

	ui->checkBox_enable_audio->setChecked(CAppConfig::GetInstance().GetPreferenceBoolean(PREFERENCE_AUDIO_ENABLEOUTPUT));
	ui->spinBox_spuBlockCount->setValue(CAppConfig::GetInstance().GetPreferenceInteger(PREF_AUDIO_SPUBLOCKCOUNT));
	ui->comboBox_presentation_mode->setCurrentIndex(CAppConfig::GetInstance().GetPreferenceInteger(PREF_CGSHANDLER_PRESENTATION_MODE));
}

//General Page ---------------------------------

void SettingsDialog::on_comboBox_system_language_currentIndexChanged(int index)
{
	CAppConfig::GetInstance().SetPreferenceInteger(PREF_SYSTEM_LANGUAGE, index);
}

void SettingsDialog::on_checkBox_limitFrameRate_clicked(bool checked)
{
	CAppConfig::GetInstance().SetPreferenceBoolean(PREF_PS2_LIMIT_FRAMERATE, checked);
}

void SettingsDialog::on_checkBox_showEECPUUsage_clicked(bool checked)
{
	CAppConfig::GetInstance().SetPreferenceBoolean(PREF_UI_SHOWEECPUUSAGE, checked);
}

void SettingsDialog::on_button_browseArcadeRomsDir_clicked()
{
	auto prevDir = PathToQString(CAppConfig::GetInstance().GetPreferencePath(PREF_PS2_ARCADEROMS_DIRECTORY));
	auto newDir = QFileDialog::getExistingDirectory(this, tr("Select Arcade ROMs Directory"), prevDir,
	                                                QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
	if(newDir.isEmpty())
	{
		return;
	}
	CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_ARCADEROMS_DIRECTORY, QStringToPath(newDir));
	ui->edit_arcadeRoms_dir->setText(newDir);
}

void SettingsDialog::on_checkBox_enableArcadeIOServer_clicked(bool checked)
{
	CAppConfig::GetInstance().SetPreferenceBoolean(PREF_PS2_ARCADE_IO_SERVER_ENABLED, checked);
}

void SettingsDialog::on_lineEdit_arcadeIOServerPort_textChanged(const QString& value)
{
	CAppConfig::GetInstance().SetPreferenceInteger(PREF_PS2_ARCADE_IO_SERVER_PORT, value.toInt());
}

void SettingsDialog::on_checkBox_useRealCardReader_clicked(bool checked)
{
	CAppConfig::GetInstance().SetPreferenceBoolean(PREF_PS2_ARCADE_USE_REAL_CARD_READER, checked);
	ui->label_cardReaderCom->setEnabled(checked);
	ui->comboBox_cardReaderCom->setEnabled(checked);
}

void SettingsDialog::on_comboBox_cardReaderCom_currentTextChanged(const QString& value)
{
	auto portName = value.trimmed().toStdString();
	CAppConfig::GetInstance().SetPreferenceString(PREF_PS2_ARCADE_CARD_READER_COM_PORT, portName.c_str());
}

//Video Page ---------------------------------

void SettingsDialog::on_checkBox_widescreenOutput_clicked(bool checked)
{
	CAppConfig::GetInstance().SetPreferenceBoolean(PREF_CGSHANDLER_WIDESCREEN, checked);
}

void SettingsDialog::on_checkBox_enable_gs_ram_reads_clicked(bool checked)
{
	CAppConfig::GetInstance().SetPreferenceBoolean(PREF_CGSHANDLER_GS_RAM_READS_ENABLED, checked);
}

void SettingsDialog::on_checkBox_force_bilinear_filtering_clicked(bool checked)
{
	CAppConfig::GetInstance().SetPreferenceBoolean(PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES, checked);
}

void SettingsDialog::on_comboBox_gs_selection_currentIndexChanged(int index)
{
	CAppConfig::GetInstance().SetPreferenceInteger(PREF_VIDEO_GS_HANDLER, index);
}

void SettingsDialog::on_comboBox_vulkan_device_currentIndexChanged(int index)
{
#if HAS_GSH_VULKAN
	auto deviceVariant = ui->comboBox_vulkan_device->itemData(index);
	auto device = deviceVariant.value<GSH_Vulkan::VULKAN_DEVICE>();
	GSH_Vulkan::CDeviceInfo::GetInstance().SetSelectedDevice(device);
#endif
}

void SettingsDialog::on_button_vulkanDeviceInfo_clicked()
{
#if HAS_GSH_VULKAN
	auto log = GSH_Vulkan::CDeviceInfo::GetInstance().GetLog();
	QMessageBox::information(this, "Vulkan Device Info", log.c_str());
#endif
}

void SettingsDialog::on_comboBox_presentation_mode_currentIndexChanged(int index)
{
	CAppConfig::GetInstance().SetPreferenceInteger(PREF_CGSHANDLER_PRESENTATION_MODE, index);
}

void SettingsDialog::on_comboBox_res_multiplyer_currentIndexChanged(int index)
{
	int factor = pow(2, index);
	CAppConfig::GetInstance().SetPreferenceInteger(PREF_CGSH_OPENGL_RESOLUTION_FACTOR, factor);
}

//Audio Page ---------------------------------

void SettingsDialog::on_checkBox_enable_audio_clicked(bool checked)
{
	CAppConfig::GetInstance().SetPreferenceBoolean(PREFERENCE_AUDIO_ENABLEOUTPUT, checked);
}

void SettingsDialog::on_spinBox_spuBlockCount_valueChanged(int value)
{
	CAppConfig::GetInstance().SetPreferenceInteger(PREF_AUDIO_SPUBLOCKCOUNT, value);
}
