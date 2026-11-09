*** Settings ***
Documentation     The controller (zone_b) cannot reach the server (zone_a); PostgreSQL is the only shared host.
Resource          resources/stack.resource
Suite Setup       Compose    exec    -T    controller    true


*** Test Cases ***
Controller cannot reach the server
    ${result}=    Open TCP From Controller    172.28.10.20    8443
    Should Not Be Equal As Integers    ${result.rc}    0    controller reached the server

Controller reaches PostgreSQL
    ${result}=    Open TCP From Controller    172.28.20.10    5432
    Should Be Equal As Integers    ${result.rc}    0    ${result.stdout}


*** Keywords ***
Open TCP From Controller
    [Arguments]    ${host}    ${port}
    ${result}=    Run Compose    exec    -T    controller
    ...    timeout    3    bash    -c    exec 3<>/dev/tcp/${host}/${port}
    RETURN    ${result}
