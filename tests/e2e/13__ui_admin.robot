*** Settings ***
Documentation     The administration panel as the admin, on a device of its own (``ui-a``): enrollment
...               and a duplicate (409), a configuration applied and one the controller rejects
...               (422 INVALID_RATE), certificateHold and its release, a key rotation and the deletion
...               of the superseded certificate, a permanent revocation, and the removal. Then a user
...               is created and deleted, and the last admin cannot be. The suite leaves the fleet
...               and the users as it found them; the teardown cleans up after a failure too.
Resource          resources/api.resource
Resource          resources/ui.resource
Suite Setup       Open Administration
Suite Teardown    Finish Administration
Test Tags         admin


*** Variables ***
${DEVICE}         ui-a
${UI_USER}        ui-user
${UI_USER_PASSWORD}    ui-user-password


*** Test Cases ***
Admin enrolls a device
    Go To Admin Tab    devices
    Click    data-testid=enroll-open
    Wait For Elements State    data-testid=dialog-enroll    visible
    Fill Text    data-testid=enroll-name    ${DEVICE}
    Select Options By    data-testid=enroll-type    value    sine
    Capture Screen    admin-enroll-dialog
    Click    data-testid=enroll-submit
    Badge Should Be    data-testid=admin-devices-result    issued
    Row Should Exist    admin-device-row-${DEVICE}
    Badge Should Be    data-testid=admin-device-row-${DEVICE} >> data-testid=badge-identity    ACTIVE
    Badge Should Be    data-testid=admin-device-row-${DEVICE} >> data-testid=badge-session    ONLINE
    Capture Screen    admin-enrolled

A duplicate name is a conflict
    Click    data-testid=enroll-open
    Fill Text    data-testid=enroll-name    ${DEVICE}
    Click    data-testid=enroll-submit
    Badge Should Be    data-testid=admin-devices-result    conflict

The new device is live in the visualization
    Go To Tab    devices
    Row Should Exist    device-row-${DEVICE}
    Wait Until Seq Advances    ${DEVICE}

The admin applies a configuration
    Go To    ${BASE_URL}/admin/config/?device=${DEVICE}
    Wait For Elements State    data-testid=config-form    visible
    Badge Should Be    data-testid=config-form >> data-testid=badge-sync    SYNC
    Fill Text    data-testid=config-rate    250
    Fill Text    data-testid=config-amplitude    2.5
    Click    data-testid=config-submit
    Badge Should Be    data-testid=config-result    applied
    Get Text    data-testid=config-form    contains    Aplicada: 250 ms
    Capture Screen    admin-config

The controller rejects an out-of-range rate
    Fill Text    data-testid=config-rate    5
    Click    data-testid=config-submit
    Badge Should Be    data-testid=config-result    rejected
    Get Text    data-testid=config-result    contains    INVALID_RATE
    Get Text    data-testid=config-form    contains    Aplicada: 250 ms
    Capture Screen    admin-config-rejected

The visualization shows the applied configuration
    Go To    ${BASE_URL}/device/?name=${DEVICE}
    Get Text    data-testid=config-applied    contains    250
    Wait For Elements State    data-testid=manage-identity    visible
    Wait For Elements State    data-testid=manage-config    visible

certificateHold suspends and can be lifted
    Click    data-testid=manage-identity
    Wait For Elements State    data-testid=page-admin-identity    visible
    Get Selected Options    data-testid=identity-device    value    ==    ${DEVICE}
    Select Options By    data-testid=identity-reason    value    certificateHold
    Click    data-testid=identity-revoke
    Badge Should Be    data-testid=identity-result    suspended
    Badge Should Be    data-testid=page-admin-identity >> data-testid=badge-identity    SUSPENDED
    Capture Screen    admin-identity-suspended
    Click    data-testid=identity-unhold
    Badge Should Be    data-testid=identity-result    unheld
    Badge Should Be    data-testid=page-admin-identity >> data-testid=badge-identity    ACTIVE
    Badge Should Be    data-testid=page-admin-identity >> data-testid=badge-session    ONLINE

Rotation issues a new certificate and the old one can be deleted
    ${old}=    Get Text    data-testid=identity-serial
    Click    data-testid=identity-rotate
    Badge Should Be    data-testid=identity-result    rotated
    Get Text    data-testid=identity-serial    !=    ${old}
    Count Should Be    css=[data-testid^="admin-cert-row-"]    ==    2
    Get Attribute    data-testid=admin-cert-row-${old}    data-current    ==    false
    Get Text    data-testid=admin-cert-row-${old}    contains    superseded
    Capture Screen    admin-identity-rotated
    Click    data-testid=cert-delete-${old}
    Wait For Elements State    data-testid=dialog-delete-cert    visible
    Click    data-testid=dialog-delete-cert-confirm
    Badge Should Be    data-testid=identity-result    cert-deleted
    Row Should Not Exist    admin-cert-row-${old}

A permanent revocation asks first
    Select Options By    data-testid=identity-reason    value    keyCompromise
    Click    data-testid=identity-revoke
    Wait For Elements State    data-testid=dialog-revoke    visible
    Capture Screen    admin-revoke-dialog
    Click    data-testid=dialog-revoke-cancel
    Wait For Elements State    data-testid=dialog-revoke    detached
    Badge Should Be    data-testid=page-admin-identity >> data-testid=badge-identity    ACTIVE
    Click    data-testid=identity-revoke
    Click    data-testid=dialog-revoke-confirm
    Badge Should Be    data-testid=identity-result    revoked
    Badge Should Be    data-testid=page-admin-identity >> data-testid=badge-identity    REVOKED
    Get Element States    data-testid=identity-rotate    contains    disabled
    Get Element States    data-testid=identity-unhold    contains    disabled

The revoked serial is in the CRL
    ${serial}=    Get Text    data-testid=identity-serial
    Go To Tab    pki
    Click    data-testid=crl-show
    Row Should Exist    crl-row-${serial}
    Get Text    data-testid=crl-row-${serial}    contains    keyCompromise

The admin deletes the device
    Go To Admin Tab    devices
    Click    data-testid=delete-${DEVICE}
    Wait For Elements State    data-testid=dialog-delete    visible
    Click    data-testid=dialog-delete-confirm
    Badge Should Be    data-testid=admin-devices-result    deleted
    Row Should Not Exist    admin-device-row-${DEVICE}
    Go To Tab    devices
    Row Should Exist    device-row-dummy1
    Should Not Be Shown    device-row-${DEVICE}

The admin creates a normal user
    Go To Admin Tab    users
    Fill Text    data-testid=user-username    ${UI_USER}
    Fill Secret    data-testid=user-password    $UI_USER_PASSWORD
    Select Options By    data-testid=user-role    value    normal
    Click    data-testid=user-create
    Badge Should Be    data-testid=users-result    created
    Row Should Exist    user-row-${UI_USER}
    Badge Should Be    data-testid=user-role-${UI_USER}    normal
    Capture Screen    admin-users

A duplicate user is a conflict
    Fill Text    data-testid=user-username    ${UI_USER}
    Fill Secret    data-testid=user-password    $UI_USER_PASSWORD
    Click    data-testid=user-create
    Badge Should Be    data-testid=users-result    conflict

The admin deletes the user
    Click    data-testid=user-delete-${UI_USER}
    Wait For Elements State    data-testid=dialog-delete-user    visible
    Click    data-testid=dialog-delete-user-confirm
    Badge Should Be    data-testid=users-result    deleted
    Row Should Not Exist    user-row-${UI_USER}

The last admin cannot be deleted
    Click    data-testid=user-delete-admin
    Click    data-testid=dialog-delete-user-confirm
    Badge Should Be    data-testid=users-result    last-admin
    Row Should Exist    user-row-admin
    Capture Screen    admin-users-last-admin


*** Keywords ***
Open Administration
    Arm Fleet
    Open Panel As Admin

Finish Administration
    Run Keyword And Ignore Error    Close Browser    ALL
    Run Keyword And Ignore Error    Remove Admin Leftovers
    Destroy Stack

Open Panel As Admin
    Open Panel
    Sign In    admin    ${ADMIN_PASSWORD}

Remove Admin Leftovers
    [Documentation]    Whatever a failed test left behind. 404 means there was nothing to remove.
    ${line}=    Get File    ${TOKEN_FILE}
    ${value}=    Evaluate    $line.strip().split(': ', 1)[1]
    &{headers}=    Create Dictionary    Authorization=${value}
    FOR    ${path}    IN    /api/devices/${DEVICE}    /api/users/${UI_USER}
        ${response}=    DELETE    ${BASE_URL}${path}    headers=${headers}    verify=${TLS_CERT}    expected_status=any
        Should Be True    $response.status_code in (200, 204, 404)    DELETE ${path}: ${response.status_code} ${response.text}
    END
