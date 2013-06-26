/**
 ******************************************************************************
 * @file       navigationwizard.h
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2013
 * @see        The GNU Public License (GPL) Version 3
 *
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup SetupWizard Setup Wizard
 * @{
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#ifndef NAVIGATIONWIZARD_H
#define NAVIGATIONWIZARD_H

#include <QWizard>
#include <coreplugin/icore.h>
#include <coreplugin/iboardtype.h>
#include <coreplugin/connectionmanager.h>
#include "vehicleconfigurationsource.h"
#include "vehicleconfigurationhelper.h"

/**
 * @brief The NavigationWizard class simplifies setting up a board for
 * navigation.
 *
 * The steps for this wizard are:
 *   1. verify failsafe works
 *   2. enable the appropriate modules and INS mode
 *      (GPS, VtolPathFollower, PathFollower)
 *   3. load the variances and other settings
 *   4. reboot
 */
class NavigationWizard : public QWizard, public VehicleConfigurationSource {
    Q_OBJECT

public:
    NavigationWizard(QWidget *parent = 0);
    int nextId() const;

    void setGPSSetting(SetupWizard::GPS_SETTING setting) { m_gpsSetting = setting; }
    NavigationWizard::GPS_SETTING getGPSSetting() const { return m_gpsSetting; }

    void setRestartNeeded(bool needed) { m_restartNeeded = needed; }
    bool isRestartNeeded() const { return m_restartNeeded; }

    Core::ConnectionManager *getConnectionManager()
    {
        if (!m_connectionManager) {
            m_connectionManager = Core::ICore::instance()->connectionManager();
            Q_ASSERT(m_connectionManager);
        }
        return m_connectionManager;
    }

private slots:
    void pageChanged(int currId);

private:
    enum { PAGE_START, PAGE_FAILSAFE, PAGE_UPLOAD_SETTINGS, PAGE_REBOOT, PAGE_END };
    void createPages();

    GPS_SETTING m_gpsSetting;

    //! Indicates the board needs to be rebooted
    bool m_restartNeeded;

    bool m_back;

    Core::ConnectionManager *m_connectionManager;
};

#endif // NAVIGATIONWIZARD_H