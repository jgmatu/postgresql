*** Settings ***
Documentation     HTTP checks with RequestsLibrary and scenarios through ``client/pgdemo.sh``.
...               Requests, commands and their output are echoed live only with
...               ``--variable E2E_DEBUG:true``.
Library           Collections
Library           OperatingSystem
Library           String
Library           RequestsLibrary
Library           LiveProcess.py
Resource          stack.resource


*** Keywords ***
HTTP Status Should Be
    [Documentation]    Send ``method`` to ``path`` and fail unless the server answers ``expected``.
    ...                Extra arguments (``headers``, ``json``, ``data``) go to RequestsLibrary.
    [Arguments]    ${expected}    ${method}    ${path}    &{kwargs}
    Console    \n> ${method} ${path} (expecting ${expected})
    Set To Dictionary    ${kwargs}    verify=${TLS_CERT}
    ${response}=    Run Keyword    ${method}    ${BASE_URL}${path}    expected_status=any    &{kwargs}
    Console    ${SPACE}${SPACE}| HTTP ${response.status_code} ${response.text}
    Should Be Equal As Integers    ${response.status_code}    ${expected}
    ...    ${method} ${path}: expected HTTP ${expected}, got ${response.status_code}: ${response.text}    values=False
    RETURN    ${response}

Auth Headers
    [Documentation]    The token file holds a ready-made ``Authorization: Bearer ...`` line.
    [Arguments]    ${content_type}=${None}
    ${line}=    Get File    ${TOKEN_FILE}
    ${line}=    Strip String    ${line}
    ${name}    ${value}=    Split String    ${line}    :${SPACE}    1
    &{headers}=    Create Dictionary    ${name}=${value}
    IF    $content_type is not None
        Set To Dictionary    ${headers}    Content-Type=${content_type}
    END
    RETURN    ${headers}

Run Client With
    [Documentation]    Run ``client/pgdemo.sh`` with ``extra`` (a dictionary) added to its environment.
    [Arguments]    ${extra}    @{args}
    ${env}=    Evaluate
    ...    {**os.environ, 'PGDEMO_URL': $BASE_URL, 'PGDEMO_TOKEN': $TOKEN_FILE, 'PGDEMO_CA': $TLS_CERT, **$extra}
    ...    modules=os
    ${result}=    Run Live    ${ROOT}${/}client${/}pgdemo.sh    @{args}    env=${env}    timeout=60s
    RETURN    ${result}

Client
    [Arguments]    @{args}
    ${result}=    Run Client With    ${{ {} }}    @{args}
    RETURN    ${result}

Client Should Succeed
    [Arguments]    @{args}
    ${result}=    Client    @{args}
    Should Be Equal As Integers    ${result.rc}    0    client ${args} failed:\n${result.stderr}    values=False
    RETURN    ${result.stdout}

Client Json
    [Documentation]    Parsed JSON body. The exit status is not checked: rejected requests still print their body.
    [Arguments]    @{args}
    ${result}=    Client    @{args}
    ${body}=    Evaluate    json.loads($result.stdout)    modules=json
    RETURN    ${body}

Login As
    [Arguments]    ${user}    ${password}
    &{extra}=    Create Dictionary    PGDEMO_PASSWORD=${password}
    ${result}=    Run Client With    ${extra}    login    ${user}
    Should Be Equal As Integers    ${result.rc}    0    login ${user} failed:\n${result.stdout}    values=False

Target Field
    [Documentation]    ``field`` of ``target`` in ``config status``.
    [Arguments]    ${target}    ${field}
    ${status}=    Client Json    config    status
    FOR    ${row}    IN    @{status}
        IF    $row['target'] == $target    RETURN    ${row}[${field}]
    END
    Fail    target ${target} missing from config status

Request Status Should Be
    [Arguments]    ${request_id}    ${expected}
    ${id}=    Convert To String    ${request_id}
    ${request}=    Client Json    config    request    ${id}
    Should Be Equal    ${request}[status]    ${expected}

Capture SSE
    [Documentation]    Three seconds of ``/api/stream`` for ``channels``.
    ...                ``timeout`` exits 124 when it stops curl, and that is the expected outcome.
    [Arguments]    ${channels}
    ${result}=    Run Live    bash    -c
    ...    timeout 3 curl -sN --cacert '${TLS_CERT}' -H @${TOKEN_FILE} '${BASE_URL}/api/stream?channels=${channels}'; test $? -eq 124
    RETURN    ${result.stdout}
