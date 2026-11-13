*** Settings ***
Documentation     The control panel in Chromium, through Robot Framework Browser (Playwright).
...
...               Elements are found by ``data-testid``; states (badges, outcomes) are read from
...               ``data-state``, which holds the API value rather than its Spanish label. Getters
...               with an assertion retry for ``retry_assertions_for`` (15 s), so a live update
...               that takes longer than the library's 1 s default still passes.
...
...               ``--variable UI_SCREENSHOTS:frontend/docs/screens`` also copies every capture
...               there (relative paths are from the repository root); frontend/README.md uses them.
Library           Browser    timeout=15s    retry_assertions_for=15s    auto_closing_level=SUITE
Library           OperatingSystem
Resource          stack.resource


*** Variables ***
${UI_SCREENSHOTS}     ${EMPTY}
${UI_BROWSER}         chromium
${UI_HEADLESS}        ${True}
${NORMAL_USER}        alice
${NORMAL_PASSWORD}    normal-user-password


*** Keywords ***
Open Panel
    [Documentation]    A fresh browser at the login page, 1440x900 and in Spanish. The server
    ...                certificate is the stack's self-signed one, so HTTPS errors are ignored.
    New Browser    ${UI_BROWSER}    headless=${UI_HEADLESS}
    New Context    viewport={'width': 1440, 'height': 900}    ignoreHTTPSErrors=True
    ...    locale=es-ES    timezoneId=Europe/Madrid
    New Page    ${BASE_URL}/
    Wait For Elements State    data-testid=login-form    visible

Login As
    [Documentation]    Sign in through the form and land on Dispositivos.
    [Arguments]    ${user}    ${password}
    Go To    ${BASE_URL}/
    Wait For Elements State    data-testid=login-form    visible
    Fill Text    data-testid=login-username    ${user}
    Fill Secret    data-testid=login-password    $password
    Click    data-testid=login-submit
    Wait For Elements State    data-testid=page-devices    visible
    Get Text    data-testid=user-menu    contains    ${user}

Logout
    Click    data-testid=user-menu
    Click    data-testid=logout
    Wait For Elements State    data-testid=login-form    visible

Go To Tab
    [Documentation]    A tab of the visualization panel (``devices``, ``config``, ``map``, ``pki``,
    ...                ``events``, ``system``, ``history``), from the navbar.
    [Arguments]    ${tab}
    Click    data-testid=nav-${tab}
    Wait For Elements State    data-testid=page-${tab}    visible
    Get Attribute    data-testid=nav-${tab}    data-current    ==    true

Go To Admin Tab
    [Documentation]    A tab of the administration panel (``devices``, ``identity``, ``config``,
    ...                ``users``): the amber navbar entry, then the sub-tab.
    [Arguments]    ${tab}
    Click    data-testid=nav-admin
    Wait For Elements State    data-testid=admin-panel    visible
    Click    data-testid=admin-tab-${tab}
    Wait For Elements State    data-testid=page-admin-${tab}    visible
    Get Attribute    data-testid=admin-tab-${tab}    aria-current    ==    page

Badge Should Be
    [Documentation]    ``data-state`` of a badge or outcome, waiting for it to get there.
    [Arguments]    ${selector}    ${state}
    Get Attribute    ${selector}    data-state    ==    ${state}

Row Should Exist
    [Arguments]    ${testid}
    Wait For Elements State    data-testid=${testid}    visible

Row Should Not Exist
    [Arguments]    ${testid}
    Wait For Elements State    data-testid=${testid}    detached

Should Not Be Shown
    [Documentation]    The element is not in the page at all, not just hidden.
    [Arguments]    ${testid}
    Get Element Count    data-testid=${testid}    ==    0

Count Should Be
    [Documentation]    Number of elements matching ``selector``, waiting for the assertion to hold:
    ...                unlike the other getters, ``Get Element Count`` does not retry on its own.
    [Arguments]    ${selector}    ${operator}    ${expected}    ${timeout}=15s
    Wait Until Keyword Succeeds    ${timeout}    0.25s
    ...    Get Element Count    ${selector}    ${operator}    ${expected}

Wait Until Seq Advances
    [Documentation]    The ``seq`` cell of ``device`` in Dispositivos moves past its current value,
    ...                driven by the SSE stream alone (the page does not poll).
    [Arguments]    ${device}
    ${cell}=    Set Variable    data-testid=device-row-${device} >> data-testid=device-seq
    Get Text    ${cell}    validate    value.isdigit()
    ${before}=    Get Text    ${cell}
    Get Text    ${cell}    validate    value.isdigit() and int(value) > ${before}

Capture Screen
    [Documentation]    Screenshot into the Robot log and, with ``UI_SCREENSHOTS``, ``<dir>/<name>.png``.
    [Arguments]    ${name}    ${full_page}=${False}
    # The navbar's current-tab underline slides with a JavaScript animation (motion).
    Sleep    0.5s
    ${path}=    Take Screenshot    filename=${OUTPUT DIR}${/}screens${/}${name}    fullPage=${full_page}
    IF    $UI_SCREENSHOTS
        ${dir}=    Evaluate    os.path.join($ROOT, $UI_SCREENSHOTS)    modules=os
        Create Directory    ${dir}
        Copy File    ${path}    ${dir}${/}${name}.png
    END
