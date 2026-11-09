*** Settings ***
Documentation     Desired/applied configuration through the Dispatcher, and controller-side rejection.
Resource          resources/api.resource


*** Test Cases ***
Valid config is applied (SUCCESS)
    ${result}=    Client Json    config    set    dummy2    rate_ms=400    amplitude=7.5
    Set Suite Variable    ${VALID}    ${result}
    Should Be Equal    ${result}[status]    SUCCESS

Dispatcher woke on config_events
    Should Be Equal    ${VALID}[dispatcher]    SUCCESS

Out-of-range rate is rejected by the controller
    ${result}=    Client Json    config    set    dummy2    rate_ms=5
    Should Be Equal    ${result}[request][error_code]    INVALID_RATE

Out-of-range amplitude is rejected
    ${result}=    Client Json    config    set    dummy2    amplitude=5000
    Should Be Equal    ${result}[request][error_code]    INVALID_AMPLITUDE

Sync view shows the error
    ${sync}=    Device Field    dummy2    sync
    Should Be Equal    ${sync}    ERROR

Bad body is 400
    ${headers}=    Auth Headers    content_type=application/json
    HTTP Status Should Be    400    POST    /api/config    headers=${headers}
    ...    data={"device":"dummy1","rate_ms":"fast"}

Unknown device is 404
    ${headers}=    Auth Headers    content_type=application/json
    HTTP Status Should Be    404    POST    /api/config    headers=${headers}
    ...    data={"device":"nope","rate_ms":500}

Burst: 20 applied
    Run Burst
    Should Contain    ${BURST}    HTTP 200 x 20

Burst: 5 rejected
    Should Contain    ${BURST}    HTTP 422 x 5

Config history records applied changes
    ${rows}=    Client Json    config    history    dummy1    limit=5
    Length Should Be    ${rows}    5


*** Keywords ***
Run Burst
    [Documentation]    25 concurrent requests; every 5th one is out of range.
    ${output}=    Client Should Succeed    burst    25    dummy1
    Set Suite Variable    ${BURST}    ${output}
