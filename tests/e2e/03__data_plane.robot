*** Settings ***
Documentation     Live rows, monthly-partitioned history and the SSE stream of both tables.
Resource          resources/api.resource
Suite Setup       Wait For Data And Capture SSE


*** Test Cases ***
Dummy1 has live rows
    ${rows}=    Client Json    data    dummy1
    Should Not Be Empty    ${rows}

Dummy2 has live rows
    ${rows}=    Client Json    data    dummy2
    Should Not Be Empty    ${rows}

History rows name their monthly partition
    ${rows}=    Client Json    history    dummy1    limit=1
    Should Match Regexp    ${rows}[0][partition]    ^dummy1_history_y[0-9]{4}m[0-9]{2}$

Unknown table is 400
    ${headers}=    Auth Headers
    HTTP Status Should Be    400    GET    /api/data/dummy3    headers=${headers}

SSE carries dummy1 events
    Should Match Regexp    ${SSE}    (?m)^event: dummy1

SSE carries dummy2 events
    Should Match Regexp    ${SSE}    (?m)^event: dummy2

SSE filter excludes config
    Should Not Match Regexp    ${SSE}    (?m)^event: config


*** Keywords ***
Live Rows Should Exist
    [Arguments]    ${table}
    ${rows}=    Client Json    data    ${table}
    Should Not Be Empty    ${rows}

Wait For Data And Capture SSE
    Wait Until Keyword Succeeds    15s    0.5s    Live Rows Should Exist    dummy1
    Wait Until Keyword Succeeds    15s    0.5s    Live Rows Should Exist    dummy2
    ${sse}=    Capture SSE    dummy1,dummy2
    Set Suite Variable    ${SSE}    ${sse}
