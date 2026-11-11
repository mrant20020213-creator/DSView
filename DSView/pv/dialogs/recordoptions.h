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

#ifndef DSVIEW_PV_RECORDOPTIONS_H
#define DSVIEW_PV_RECORDOPTIONS_H

#include <QDialogButtonBox>
#include "dsdialog.h"

class QCheckBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QLabel;

namespace pv {
namespace dialogs {

// The settings of writing the captured logic data to files continuously.
class RecordOptionsDlg : public DSDialog
{
    Q_OBJECT

public:
    RecordOptionsDlg(QWidget *parent);

    inline bool is_done(){
        return _bDone;
    }

protected:
    void accept();
    void reject();

private Q_SLOTS:
    void on_browse();
    void on_enable_changed();

private:
    QCheckBox   *_enable_box;
    QLabel      *_dir_label;
    QLineEdit   *_dir_edit;
    QPushButton *_browse_button;
    QLabel      *_size_label;
    QSpinBox    *_size_spinBox;
    QDialogButtonBox _button_box;
    bool        _bDone;
};

} // namespace dialogs
} // namespace pv

#endif // DSVIEW_PV_RECORDOPTIONS_H
