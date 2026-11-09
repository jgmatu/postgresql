*** Settings ***
Documentation     Seeded devices, monthly-partitioned telemetry and the SSE stream of both telemetry channels.
Resource          resources/api.resource
Suite Setup       Wait For Data And Capture SSE


*** Test Cases ***
The three seeded devices are listed
    ${devices}=    Client Json    devices
    ${names}=    Evaluate    [d['device'] for d in $devices]
    Should Contain    ${names}    dummy1
    Should Contain    ${names}    dummy2
    Should Contain    ${names}    dummy3

Dummy1 has a last sample
    ${dev}=    Client Json    data    dummy1
    Should Be Equal    ${dev}[payload]    scalar
    Should Not Be Equal    ${dev}[value]    ${None}

Dummy3 has a last position
    ${dev}=    Client Json    data    dummy3
    Should Be Equal    ${dev}[payload]    gps
    Should Be True    ${dev}[lat] > 40

Device carries its config sync state
    ${dev}=    Client Json    data    dummy2
    Should Be Equal    ${dev}[config][sync]    SYNC

Scalar telemetry names its monthly partition
    ${rows}=    Client Json    history    dummy1    limit=1
    Should Match Regexp    ${rows}[0][partition]    ^telemetry_scalar_y\\d{4}m\\d{2}$

GPS telemetry names its monthly partition
    ${rows}=    Client Json    history    dummy3    limit=1
    Should Match Regexp    ${rows}[0][partition]    ^telemetry_gps_y\\d{4}m\\d{2}$

Unknown device is 404
    ${headers}=    Auth Headers
    HTTP Status Should Be    404    GET    /api/devices/nope    headers=${headers}

Malformed device name is 400
    ${headers}=    Auth Headers
    HTTP Status Should Be    400    GET    /api/devices/9bad    headers=${headers}

Unknown SSE channel is 400
    ${headers}=    Auth Headers
    HTTP Status Should Be    400    GET    /api/stream?channels\=dummy1    headers=${headers}

SSE carries telemetry_scalar events
    Should Match Regexp    ${SSE}    (?m)^event: telemetry_scalar

SSE carries telemetry_gps events
    Should Match Regexp    ${SSE}    (?m)^event: telemetry_gps

SSE payload names the device
    Should Contain    ${SSE}    "device":"dummy1"
    Should Contain    ${SSE}    "device":"dummy3"

SSE filter excludes config
    Should Not Match Regexp    ${SSE}    (?m)^event: config


*** Keywords ***
Device Should Have A Sample
    [Arguments]    ${device}
    ${dev}=    Client Json    data    ${device}
    Should Not Be Equal    ${dev}[seq]    ${None}

Wait For Data And Capture SSE
    Wait Until Keyword Succeeds    15s    0.5s    Device Should Have A Sample    dummy1
    Wait Until Keyword Succeeds    15s    0.5s    Device Should Have A Sample    dummy2
    Wait Until Keyword Succeeds    15s    0.5s    Device Should Have A Sample    dummy3
    ${sse}=    Capture SSE    channels=telemetry_scalar,telemetry_gps
    Set Suite Variable    ${SSE}    ${sse}
