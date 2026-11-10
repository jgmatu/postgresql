*** Settings ***
Documentation     Every visualization tab as the normal user: the live device list, the detail with its
...               chart and history, configuration, the GPS map, PKI with the CRL, the event stream
...               with server-side filters, the system status, and the read-only audit history.
...               Read-only: no "Gestionar" links.
Resource          resources/ui.resource
Suite Setup       Open Panel And Login
Suite Teardown    Close Browser    ALL


*** Test Cases ***
Dispositivos lists the seeds online
    FOR    ${name}    IN    dummy1    dummy2    dummy3
        Row Should Exist    device-row-${name}
        Badge Should Be    data-testid=device-row-${name} >> data-testid=badge-session    ONLINE
        Badge Should Be    data-testid=device-row-${name} >> data-testid=badge-identity    ACTIVE
    END
    Badge Should Be    data-testid=stream-state    open
    Should Not Be Shown    manage-devices

Telemetry arrives live over SSE
    Wait Until Seq Advances    dummy1
    Wait Until Seq Advances    dummy3
    Get Text    data-testid=stat-events >> div >> nth=1    validate    int(value) > 0
    Get Text    data-testid=device-row-dummy3 >> data-testid=device-value    matches    ^40,\\d+, -3,\\d+$
    Capture Screen    devices

The device detail draws the chart and the history
    Click    data-testid=device-link-dummy1
    Wait For Elements State    data-testid=device-title    visible
    Get Text    data-testid=device-title    contains    dummy1
    Get Url    contains    /device/?name=dummy1
    Get Attribute    data-testid=nav-devices    data-current    ==    true
    ${points}=    Get Attribute    data-testid=chart    data-points
    Get Attribute    data-testid=chart    data-points    validate    int(value) > ${points}
    Count Should Be    data-testid=history-table >> tbody tr    >    0
    Get Text    data-testid=history-partition >> nth=0    matches    ^telemetry_scalar_y
    Count Should Be    data-testid=certificates-table >> tbody tr    >    0
    Count Should Be    data-testid=enrollments-table >> tbody tr    >    0
    Wait For Elements State    data-testid=config-applied    visible
    Should Not Be Shown    manage-identity
    Should Not Be Shown    manage-config
    Capture Screen    device-detail

The history size can be changed
    [Documentation]    The chart plots the whole window; the table lists the latest rows of it.
    Select Options By    data-testid=history-limit    value    200
    Get Selected Options    data-testid=history-limit    value    ==    200
    Get Attribute    data-testid=chart    data-points    validate    int(value) > 50

A GPS device shows its track
    Go To    ${BASE_URL}/device/?name=dummy3
    Get Attribute    data-testid=gps-map >> data-testid=gps-track-dummy3    data-fixes    validate    int(value) > 1

An unknown device says so
    Go To    ${BASE_URL}/device/?name=no-such-device
    Wait For Elements State    data-testid=device-not-found    visible

Configuración shows applied and desired state
    Go To Tab    config
    FOR    ${name}    IN    dummy1    dummy2    dummy3
        Row Should Exist    config-row-${name}
    END
    Get Attribute    data-testid=config-row-dummy1 >> data-testid=badge-sync    data-state
    ...    validate    value in ('SYNC', 'PENDING', 'ERROR', 'UNINITIALISED')
    Wait For Elements State    data-testid=config-history-table    visible
    Should Not Be Shown    manage-config
    Capture Screen    config

A configuration request can be looked up by id
    ${id}=    Get Attribute    css=[data-testid="config-table"] [data-request-id] >> nth=0    data-request-id
    Fill Text    data-testid=request-id    ${id}
    Click    data-testid=request-find
    Get Text    data-testid=request-result    contains    Petición #${id}
    Get Attribute    data-testid=request-result    data-state    validate    value in ('PENDING', 'SUCCESS', 'ERROR', 'TIMEOUT')
    Fill Text    data-testid=request-id    999999999
    Click    data-testid=request-find
    Wait For Elements State    data-testid=request-not-found    visible

Mapa follows the GPS device around the origin
    Go To Tab    map
    Row Should Exist    gps-row-dummy3
    Get Attribute    data-testid=gps-track-dummy3    data-fixes    validate    int(value) > 1
    ${before}=    Get Attribute    data-testid=gps-track-dummy3 >> circle    cx
    Get Attribute    data-testid=gps-track-dummy3 >> circle    cx    !=    ${before}
    Capture Screen    map

PKI shows the CA, the CRL and the enrollments
    Go To Tab    pki
    Get Text    data-testid=stat-ca >> div >> nth=1    matches    ^device-root-
    ${ca}=    Published CA
    Get Attribute    css=[data-testid^="root-row-"][data-signing="true"] >> data-testid=root-fingerprint
    ...    data-fingerprint    ==    ${ca}[signing_fingerprint]
    Click    data-testid=crl-show
    Get Text    data-testid=crl-pem    starts    -----BEGIN X509 CRL-----
    Count Should Be    data-testid=enrollments-table >> tbody tr    >    0
    Capture Screen    pki

The CRL downloads as a PEM file
    ${download}=    Promise To Wait For Download
    Click    data-testid=crl-download
    ${file}=    Wait For    ${download}
    Should Be Equal    ${file}[suggestedFilename]    pgdemo-crl.pem
    ${pem}=    Get File    ${file}[saveAs]
    Should Start With    ${pem}    -----BEGIN X509 CRL-----

Eventos streams every channel
    Go To Tab    events
    Badge Should Be    data-testid=stream-state    open
    Count Should Be    data-testid=event-row    >    3
    Capture Screen    events

Eventos filters by device on the server
    Fill Text    data-testid=filter-devices    dummy3
    Click    data-testid=events-clear
    Count Should Be    data-testid=event-row    >    1
    Count Should Be    css=[data-testid="event-row"]:not([data-device="dummy3"])    ==    0
    Count Should Be    css=[data-testid="event-row"][data-channel="telemetry_gps"]    >    0

Eventos filters by channel on the server
    Fill Text    data-testid=filter-devices    ${EMPTY}
    Uncheck Checkbox    data-testid=filter-telemetry_gps
    Click    data-testid=events-clear
    Count Should Be    css=[data-testid="event-row"][data-channel="telemetry_scalar"]    >    1
    Count Should Be    css=[data-testid="event-row"][data-channel="telemetry_gps"]    ==    0
    Check Checkbox    data-testid=filter-telemetry_gps

Pausing keeps the list still
    Click    data-testid=events-pause
    Click    data-testid=events-clear
    Sleep    2s
    Count Should Be    data-testid=event-row    ==    0
    Click    data-testid=events-pause
    Count Should Be    data-testid=event-row    >    0

Sistema reports health, PostgreSQL and the session
    Go To Tab    system
    Badge Should Be    data-testid=badge-health    ok
    Badge Should Be    data-testid=badge-listener    true
    Get Text    data-testid=pg-version    contains    PostgreSQL
    Badge Should Be    data-testid=system-role    normal
    Get Text    data-testid=panel-version    matches    ^[0-9a-f]{12}(-dirty)? · \\d{4}-
    Capture Screen    system

Histórico reads dummy1 samples for the session role
    Go To Tab    history
    Get Text    data-testid=page-history    contains    demo_normal
    Wait For Elements State    data-testid=history-device >> option[value="dummy1"]    visible
    Select Options By    data-testid=history-device    value    dummy1
    Wait For Elements State    data-testid=sample-history-table    visible
    Get Text    data-testid=sample-partition >> nth=0    matches    ^telemetry_scalar_y
    Count Should Be    data-testid=applied-history-table >> tbody tr    >    0
    Count Should Be    data-testid=enrollment-history-table >> tbody tr    >    0


*** Keywords ***
Open Panel And Login
    Open Panel
    Login As    ${NORMAL_USER}    ${NORMAL_PASSWORD}

Published CA
    [Documentation]    ``GET /api/pki/ca`` straight from the API, to compare with what the page shows.
    ${line}=    Get File    ${TOKEN_FILE}
    ${value}=    Evaluate    $line.strip().split(': ', 1)[1]
    &{headers}=    Create Dictionary    Authorization=${value}
    ${response}=    GET    ${BASE_URL}/api/pki/ca    headers=${headers}    verify=${TLS_CERT}    expected_status=200
    RETURN    ${response.json()}
