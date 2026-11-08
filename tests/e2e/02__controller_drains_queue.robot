*** Settings ***
Documentation     The controller starts and applies the request queued while it was down.
Resource          resources/api.resource
Suite Setup       Start Service    controller


*** Test Cases ***
Queued request applied on controller start-up
    Wait Until Keyword Succeeds    30s    0.5s    Request Status Should Be    ${QUEUED_ID}    SUCCESS

Applied rate is the queued one
    ${rate}=    Target Field    dummy1    applied_rate_ms
    Should Be Equal As Integers    ${rate}    300
