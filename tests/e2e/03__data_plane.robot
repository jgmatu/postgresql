*** Settings ***
Documentation     Live rows, monthly-partitioned history and the SSE stream of the data tables.
Resource          resources/api.resource
Suite Setup       Wait For Data And Capture SSE


*** Test Cases ***
Dummy1 has live rows
    ${rows}=    Client Json    data    dummy1
    Should Not Be Empty    ${rows}

Dummy2 has live rows
    ${rows}=    Client Json    data    dummy2
    Should Not Be Empty    ${rows}

Dummy3 has live rows
    ${rows}=    Client Json    data    dummy3
    Should Not Be Empty    ${rows}
    Should Be True    ${rows}[0][lat] > 40

History rows name their monthly partition
    ${rows}=    Client Json    history    dummy1    limit=1
    Should Match Regexp    ${rows}[0][partition]    ^dummy1_history_y[0-9]{4}m[0-9]{2}$

Dummy3 history names its monthly partition
    ${rows}=    Client Json    history    dummy3    limit=1
    Should Match Regexp    ${rows}[0][partition]    ^dummy3_history_y[0-9]{4}m[0-9]{2}$

Unknown table is 400
    ${headers}=    Auth Headers
    HTTP Status Should Be    400    GET    /api/data/nope    headers=${headers}

SSE carries dummy1 events
    Should Match Regexp    ${SSE}    (?m)^event: dummy1

SSE carries dummy2 events
    Should Match Regexp    ${SSE}    (?m)^event: dummy2

SSE carries dummy3 events
    Should Match Regexp    ${SSE}    (?m)^event: dummy3

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
    Wait Until Keyword Succeeds    15s    0.5s    Live Rows Should Exist    dummy3
    ${sse}=    Capture SSE    dummy1,dummy2,dummy3
    Set Suite Variable    ${SSE}    ${sse}
