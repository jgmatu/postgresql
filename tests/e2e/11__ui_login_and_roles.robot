*** Settings ***
Documentation     The panel in a browser: the login page is the way in, a normal user sees only the
...               visualization tabs, and the administration entry (amber, with the role badge)
...               appears for the admin alone. Hiding it is presentation: the AdminGate sends a
...               normal user back, and 10__panel showed the API refusing the writes anyway.
...               A token the server no longer accepts ends the session.
Resource          resources/api.resource
Resource          resources/ui.resource
Suite Setup       Open A Fresh Panel
Suite Teardown    Close The Panel


*** Test Cases ***
The login page is the way in
    Get Title    ==    pgdemo · Panel de control
    Get Attribute    data-testid=login-submit    type    ==    submit
    Capture Screen    login

A page without a session sends the browser to the login
    Go To    ${BASE_URL}/devices/
    Wait For Elements State    data-testid=login-form    visible
    Get Url    matches    /$

Wrong password is refused
    Fill Text    data-testid=login-username    admin
    Fill Text    data-testid=login-password    not-the-password
    Click    data-testid=login-submit
    Get Text    data-testid=login-error    contains    Usuario o contraseña incorrectos
    Get Element Count    data-testid=page-devices    ==    0
    Capture Screen    login-error

A normal user sees the visualization tabs only
    Sign In    ${NORMAL_USER}    ${NORMAL_PASSWORD}
    FOR    ${tab}    IN    devices    config    map    pki    events    system    history
        Wait For Elements State    data-testid=nav-${tab}    visible
    END
    Get Attribute    data-testid=nav-devices    data-current    ==    true
    Should Not Be Shown    nav-admin
    Should Not Be Shown    manage-devices
    Capture Screen    devices-normal

The user menu shows the role and its PostgreSQL group
    Click    data-testid=user-menu
    Get Text    data-testid=user-name    ==    ${NORMAL_USER}
    Badge Should Be    data-testid=user-menu-header >> data-testid=badge-role    normal
    Get Text    data-testid=user-pg-role    ==    demo_normal
    Capture Screen    user-menu-normal
    Keyboard Key    press    Escape
    Wait For Elements State    data-testid=user-menu-header    detached

A normal user is sent back from the administration panel
    Go To    ${BASE_URL}/admin/users/
    Wait For Elements State    data-testid=page-devices    visible
    Get Url    contains    /devices/
    Should Not Be Shown    admin-panel

Logout ends the session
    Logout
    Go To    ${BASE_URL}/system/
    Wait For Elements State    data-testid=login-form    visible

The admin sees the administration entry
    Sign In    admin    ${ADMIN_PASSWORD}
    Get Text    data-testid=nav-admin    contains    Administración
    Get Text    data-testid=nav-admin    contains    admin
    Wait For Elements State    data-testid=manage-devices    visible
    Click    data-testid=user-menu
    Badge Should Be    data-testid=user-menu-header >> data-testid=badge-role    admin
    Get Text    data-testid=user-pg-role    ==    demo_admin
    Capture Screen    user-menu-admin
    Keyboard Key    press    Escape
    Wait For Elements State    data-testid=user-menu-header    detached

Gestionar opens the administration panel
    Click    data-testid=manage-devices
    Wait For Elements State    data-testid=page-admin-devices    visible
    Get Attribute    data-testid=nav-admin    data-current    ==    true
    Get Attribute    data-testid=admin-tab-devices    aria-current    ==    page
    Get Element Count    css=[data-current="true"][data-testid^="nav-"]    ==    1
    Capture Screen    admin-devices

A token the server rejects ends the session
    [Documentation]    The stored session claims admin with a forged token: /api/auth/me answers
    ...                401, and the panel goes back to the login saying why.
    ${forged}=    Evaluate
    ...    json.dumps({'token': 'forged.token.value', 'username': 'admin', 'role': 'admin', 'expiresAt': (time.time() + 3600) * 1000})
    ...    modules=json,time
    Evaluate JavaScript    ${None}    (s) => sessionStorage.setItem('pgdemo.session', s)    arg=${forged}
    Go To    ${BASE_URL}/admin/devices/
    Wait For Elements State    data-testid=login-expired    visible
    Get Url    contains    expired=1
    ${stored}=    Evaluate JavaScript    ${None}    () => sessionStorage.getItem('pgdemo.session')
    Should Be Equal    ${stored}    ${None}
    Capture Screen    login-expired


*** Keywords ***
Open A Fresh Panel
    Start Stack
    Login As    admin    ${ADMIN_PASSWORD}
    Ensure Alice
    Open Panel
