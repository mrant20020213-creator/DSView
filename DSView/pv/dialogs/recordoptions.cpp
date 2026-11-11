/*
 * This file is part of the DSView project.
 * DSView is based on PulseView.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "recordoptions.h"

#include <QGridLayout>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QLabel>
#include <QFileDialog>
#include <QDir>

#include "../config/appconfig.h"
#include "../ui/langresource.h"
#include "../ui/msgbox.h"

namespace pv {
namespace dialogs {

RecordOptionsDlg::RecordOptionsDlg(QWidget *parent) :
    DSDialog(parent),
    _button_box(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        Qt::Horizontal, this)
{
    _bDone = false;

    AppConfig &app = AppConfig::Instance();

    setMinimumWidth(460);

    _enable_box = new QCheckBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RECORD_ENABLE), 
                        "Write the captured data to files until stopped"), this);
    _enable_box->setChecked(app.userHistory.recordEnable);

    _dir_label = new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RECORD_DIR), "Folder: "), this);
    _dir_edit = new QLineEdit(this);
    _dir_edit->setReadOnly(true);
    _dir_edit->setText(app.userHistory.recordDir);
    _browse_button = new QPushButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RECORD_BROWSE), "Browse..."), this);

    _size_label = new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RECORD_FILE_SIZE), "Max file size(MB): "), this);
    _size_spinBox = new QSpinBox(this);
    _size_spinBox->setRange(16, 2048);
    _size_spinBox->setSingleStep(64);
    _size_spinBox->setValue(app.userHistory.recordSegmentMB);

    QLabel *tip_label = new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RECORD_TIP), 
                        "Only for logic analyzer in stream mode. The capture runs in loop mode,\n"
                        "the data is saved as a sequence of .dsl files in a new sub folder."), this);

    QGridLayout *glayout = new QGridLayout();
    glayout->setVerticalSpacing(8);
    glayout->addWidget(_enable_box, 0, 0, 1, 3);
    glayout->addWidget(_dir_label, 1, 0);
    glayout->addWidget(_dir_edit, 1, 1);
    glayout->addWidget(_browse_button, 1, 2);
    glayout->addWidget(_size_label, 2, 0);
    glayout->addWidget(_size_spinBox, 2, 1);
    glayout->addWidget(tip_label, 3, 0, 1, 3);
    glayout->addWidget(&_button_box, 4, 1, 1, 2, Qt::AlignRight);
    glayout->setColumnStretch(1, 1);

    layout()->addLayout(glayout);
    setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RECORD_TITLE), "Record to Files"));

    connect(&_button_box, SIGNAL(accepted()), this, SLOT(accept()));
    connect(&_button_box, SIGNAL(rejected()), this, SLOT(reject()));
    connect(_browse_button, SIGNAL(clicked()), this, SLOT(on_browse()));
    connect(_enable_box, SIGNAL(clicked()), this, SLOT(on_enable_changed()));

    on_enable_changed();
}

void RecordOptionsDlg::on_enable_changed()
{
    bool bEnable = _enable_box->isChecked();
    _dir_edit->setEnabled(bEnable);
    _browse_button->setEnabled(bEnable);
    _size_spinBox->setEnabled(bEnable);
}

void RecordOptionsDlg::on_browse()
{
    QString dir = QFileDialog::getExistingDirectory(this,
                        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RECORD_SELECT_DIR), "Select the record folder"),
                        _dir_edit->text(),
                        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!dir.isEmpty()){
        _dir_edit->setText(QDir::toNativeSeparators(dir));
    }
}

void RecordOptionsDlg::accept()
{
    using namespace Qt;

    QString dir = _dir_edit->text().trimmed();

    if (_enable_box->isChecked() && (dir.isEmpty() || !QDir(dir).exists())){
        MsgBox::Show(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_RECORD_NO_DIR), "Please select a folder to store the record files."));
        return;
    }

    AppConfig &app = AppConfig::Instance();
    app.userHistory.recordEnable = _enable_box->isChecked();
    app.userHistory.recordDir = dir;
    app.userHistory.recordSegmentMB = _size_spinBox->value();
    app.SaveHistory();

    _bDone = true;
    QDialog::accept();
}

void RecordOptionsDlg::reject()
{
    using namespace Qt;

    QDialog::reject();
}

} // namespace dialogs
} // namespace pv
