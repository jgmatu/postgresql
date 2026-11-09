*** Settings ***
Documentation     A device enrolled through the API: the controller starts its process, which generates
...               a key and a CSR, the server signs it, and the process proves it holds the key.
...               Its telemetry reaches an SSE stream filtered to that device, it is reconfigured,
...               and its removal keeps the telemetry. No schema change, no restart.
Resource          resources/api.resource
Library           Process
Suite Setup       Login And Watch Device
Suite Teardown    Terminate All Processes


*** Variables ***
${DEVICE}         sensor-a
${SSE_FILE}       ${WORK}${/}sse-${DEVICE}.txt


*** Test Cases ***
Admin enrolls a device (201 once its certificate is issued)
    ${headers}=    Auth Headers
    &{body}=    Create Dictionary    name=${DEVICE}    type=sine
    ${response}=    HTTP Status Should Be    201    POST    /api/devices    headers=${headers}    json=${body}
    ${result}=    Set Variable    ${response.json()}
    Should Be Equal    ${result}[status]    ISSUED
    Should Be Equal    ${result}[enrollment][kind]    ENROLL
    Should Be Equal    ${result}[device][device]    ${DEVICE}
    Should Be Equal    ${result}[device][payload]    scalar
    Should Match Regexp    ${result}[device][certificate][serial]    ^[0-9a-f]{1,40}$
    Should Be Equal    ${result}[device][identity]    ACTIVE

The device process authenticates with its own key
    Wait Until Keyword Succeeds    15s    0.5s    Session Should Be    ${DEVICE}    ONLINE
    ${dev}=    Client Json    data    ${DEVICE}
    Should Be True    ${dev}[process][pid] > 0
    Compose    exec    -T    controller    test    -f    /app/run/devices/${DEVICE}/key.pem

Duplicate device name is 409
    ${headers}=    Auth Headers
    &{body}=    Create Dictionary    name=${DEVICE}    type=sawtooth
    HTTP Status Should Be    409    POST    /api/devices    headers=${headers}    json=${body}

Unknown device type is 400
    ${headers}=    Auth Headers
    &{body}=    Create Dictionary    name=sensor-b    type=thermometer
    HTTP Status Should Be    400    POST    /api/devices    headers=${headers}    json=${body}

Controller initialises the type defaults
    Wait Until Keyword Succeeds    15s    0.5s    Device Field Should Be    ${DEVICE}    sync    SYNC
    ${rate}=    Device Field    ${DEVICE}    applied_rate_ms
    Should Be Equal As Integers    ${rate}    500

Controller writes its telemetry
    Wait Until Keyword Succeeds    15s    0.5s    Device Should Have A Sample    ${DEVICE}
    ${rows}=    Client Json    history    ${DEVICE}    limit=1
    Should Match Regexp    ${rows}[0][partition]    ^telemetry_scalar_y\\d{4}m\\d{2}$

Filtered SSE carries only this device
    ${sse}=    Wait For Device Stream
    Should Match Regexp    ${sse}    (?m)^event: device
    Should Match Regexp    ${sse}    (?m)^event: config
    Should Match Regexp    ${sse}    (?m)^event: telemetry_scalar
    Should Contain    ${sse}    "device":"${DEVICE}"
    Should Not Contain    ${sse}    "device":"dummy1"
    Should Not Match Regexp    ${sse}    (?m)^event: telemetry_gps

Device is reconfigured through the Dispatcher
    ${result}=    Client Json    config    set    ${DEVICE}    rate_ms=200    amplitude=3
    Should Be Equal    ${result}[status]    SUCCESS
    ${rate}=    Device Field    ${DEVICE}    applied_rate_ms
    Should Be Equal As Integers    ${rate}    200

Admin removes the device (200)
    ${result}=    Client Json    devices    del    ${DEVICE}
    Should Be Equal    ${result}[deleted]    ${DEVICE}

Removed device is 404
    ${headers}=    Auth Headers
    HTTP Status Should Be    404    GET    /api/devices/${DEVICE}    headers=${headers}
    HTTP Status Should Be    404    DELETE    /api/devices/${DEVICE}    headers=${headers}

Removed device cannot be configured (404)
    ${headers}=    Auth Headers    content_type=application/json
    HTTP Status Should Be    404    POST    /api/config    headers=${headers}
    ...    data={"device":"${DEVICE}","rate_ms":500}

Removed device leaves the config status
    ${status}=    Client Json    config    status
    ${names}=    Evaluate    [row['device'] for row in $status]
    Should Not Contain    ${names}    ${DEVICE}
    Should Contain    ${names}    dummy1

Removed device loses its process and its key
    Wait Until Keyword Succeeds    15s    0.5s    Controller Path Should Not Exist    /app/run/devices/${DEVICE}


*** Keywords ***
Login And Watch Device
    [Documentation]    Admin token, then an SSE stream filtered to a device that does not exist yet.
    Login As    admin    ${ADMIN_PASSWORD}
    Start Process    timeout    8    curl    -sN    --cacert    ${TLS_CERT}    -H    @${TOKEN_FILE}
    ...    ${BASE_URL}/api/stream?devices\=${DEVICE}    stdout=${SSE_FILE}    alias=sse
    Sleep    1s

Wait For Device Stream
    ${result}=    Wait For Process    sse    timeout=15s    on_timeout=terminate
    ${sse}=    Get File    ${SSE_FILE}
    Console    ${sse}
    RETURN    ${sse}

Device Field Should Be
    [Arguments]    ${device}    ${field}    ${expected}
    ${value}=    Device Field    ${device}    ${field}
    Should Be Equal As Strings    ${value}    ${expected}

Device Should Have A Sample
    [Arguments]    ${device}
    ${dev}=    Client Json    data    ${device}
    Should Not Be Equal    ${dev}[seq]    ${None}
