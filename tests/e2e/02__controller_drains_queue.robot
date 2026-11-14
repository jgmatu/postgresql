*** Settings ***
Documentation     The controller starts: the seeds and the enrollment queued while it was down get their
...               certificates, and a configuration queued while it is stopped is applied on restart.
Resource          resources/api.resource
Suite Setup       Queue An Enrollment And Start The Controller
Suite Teardown    Destroy Stack


*** Test Cases ***
Seeds enroll on controller start-up
    Wait Until Keyword Succeeds    30s    0.5s    Seeds Should Be Online
    FOR    ${name}    IN    dummy1    dummy2    dummy3
        Identity Should Be    ${name}    ACTIVE
    END

Each seed has one certificate, issued by the device CA
    ${ca}=    Client Json    pki    ca
    FOR    ${name}    IN    dummy1    dummy2    dummy3
        ${certs}=    Client Json    devices    certs    ${name}
        Length Should Be    ${certs}    1
        Should Be True    ${certs}[0][current]
        Should Be Equal    ${certs}[0][subject_cn]    ${name}
        Should Be Equal    ${certs}[0][ca]    ${ca}[roots][0][name]
    END

Device keys stay in the controller zone
    Compose    exec    -T    controller    test    -f    /app/run/devices/dummy1/key.pem
    ${mode}=    Compose    exec    -T    controller    stat    -c    %a    /app/run/devices/dummy1/key.pem
    Should Be Equal    ${mode.stdout.strip()}    600
    ${result}=    Run Compose    exec    -T    server    test    -e    /app/run/devices/dummy1
    Should Not Be Equal As Integers    ${result.rc}    0    the server container sees device keys

Enrollment queued while the controller was down is issued
    Wait Until Keyword Succeeds    30s    0.5s    Enrollment Status Should Be    ${QUEUED_ID}    ISSUED
    Client Should Succeed    devices    del    early-1

Config queued while the controller is stopped is applied on restart
    Stop Services    controller
    ${queued}=    Client Json    config    set    dummy1    rate_ms=300
    Should Be Equal    ${queued}[status]    PENDING
    Start Service    controller
    Wait Until Keyword Succeeds    30s    0.5s    Request Status Should Be    ${queued}[request_id]    SUCCESS
    ${rate}=    Device Field    dummy1    applied_rate_ms
    Should Be Equal As Integers    ${rate}    300
    Wait Until Keyword Succeeds    30s    0.5s    Seeds Should Be Online


*** Keywords ***
Queue An Enrollment And Start The Controller
    [Documentation]    The enrollment is inserted while the controller is stopped, then the controller
    ...                starts and drains it together with the seed devices.
    Start Stack
    Login As    admin    ${ADMIN_PASSWORD}
    ${headers}=    Auth Headers
    &{body}=    Create Dictionary    name=early-1    type=sine
    ${response}=    HTTP Status Should Be    202    POST    /api/devices    headers=${headers}    json=${body}
    Set Suite Variable    ${QUEUED_ID}    ${response.json()}[request_id]
    Start Service    controller
    Wait Until Keyword Succeeds    60s    0.5s    Seeds Should Be Online
