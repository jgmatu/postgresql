*** Settings ***
Documentation     Disposable docker compose stack for the end-to-end suites.
...
...               Every command runs under its own project name, so ``down -v`` never
...               touches the volumes of the development stack (project ``pgdemo``).
...               Compose output reaches the console only with ``--variable E2E_DEBUG:true``.
Library           OperatingSystem
Library           RequestsLibrary
Library           LiveProcess.py


*** Variables ***
${ROOT}               ${{ os.path.normpath(os.path.join(r'${CURDIR}', '..', '..', '..')) }}
${PROJECT}            pgdemo-e2e
${COMPOSE_FILE}       ${ROOT}${/}docker${/}compose.yaml
# Under the repository, not /tmp: a snap-packaged docker CLI cannot see the host /tmp.
${WORK}               ${ROOT}${/}run${/}e2e
${ENV_FILE}           ${WORK}${/}compose.env
${TOKEN_FILE}         ${WORK}${/}token
${TLS_CERT}           ${WORK}${/}tls.crt
${BASE_URL}           https://127.0.0.1:8080
${BUILD_TIMEOUT}      30 min
# Live command and response output. Override: robot --variable E2E_DEBUG:true
${E2E_DEBUG}          ${False}


*** Keywords ***
Run Compose
    [Documentation]    ``docker compose`` on the e2e project; returns the result without checking it.
    ...                Compose prefers shell variables over ``--env-file``, so the generated
    ...                passwords are removed from the inherited environment.
    [Arguments]    @{args}    ${timeout}=5 min
    ${env}=    Evaluate
    ...    {k: v for k, v in os.environ.items() if k not in ('POSTGRES_PASSWORD', 'DEMO_ADMIN_PASSWORD')}
    ...    modules=os
    ${result}=    Run Live    docker    compose    --progress    plain    -p    ${PROJECT}
    ...    -f    ${COMPOSE_FILE}    --env-file    ${ENV_FILE}    @{args}
    ...    cwd=${ROOT}    env=${env}    timeout=${timeout}
    RETURN    ${result}

Compose
    [Documentation]    ``docker compose`` on the e2e project; fails unless it exits with 0.
    [Arguments]    @{args}    ${timeout}=5 min
    ${result}=    Run Compose    @{args}    timeout=${timeout}
    Should Be Equal As Integers    ${result.rc}    0
    ...    docker compose ${args} failed (rc ${result.rc}):\n${result.stdout}\n${result.stderr}    values=False
    RETURN    ${result}

Start Stack
    [Documentation]    Fresh database and the server alone; the controller is started by its own suite.
    ...                The image copies ``webroot/``, so the panel must be there before it is built.
    File Should Exist    ${ROOT}${/}webroot${/}index.html
    ...    webroot/index.html missing: ejecuta scripts/fetch-webroot.sh o pnpm build en frontend/
    Remove Directory    ${WORK}    recursive=True
    Create Directory    ${WORK}
    ${postgres_password}=    Evaluate    secrets.token_hex(16)    modules=secrets
    ${admin_password}=    Evaluate    secrets.token_hex(16)    modules=secrets
    Create File    ${ENV_FILE}
    ...    POSTGRES_PASSWORD=${postgres_password}\nDEMO_ADMIN_PASSWORD=${admin_password}\n
    Set Global Variable    ${ADMIN_PASSWORD}    ${admin_password}
    Compose    down    -v    --remove-orphans
    Compose    up    -d    --build    --wait    server    timeout=${BUILD_TIMEOUT}
    Wait Until Keyword Succeeds    60s    0.5s
    ...    Compose    cp    server:/app/run/keys/tls.crt    ${TLS_CERT}
    Wait For Health

Wait For Health
    Console    \n> waiting for ${BASE_URL}/api/health
    Wait Until Keyword Succeeds    60s    0.5s
    ...    GET    ${BASE_URL}/api/health    expected_status=200    verify=${TLS_CERT}

Start Service
    [Documentation]    ``init`` has already completed; ``--no-deps`` keeps compose from running it again.
    [Arguments]    ${service}
    Compose    up    -d    --no-deps    --wait    ${service}

Stop Services
    [Arguments]    @{services}
    Compose    stop    @{services}

Copy Service Logs
    [Documentation]    ``run/logs`` lives in the ``demo-run`` volume shared by server and controller.
    [Arguments]    ${destination}
    Remove Directory    ${destination}    recursive=True
    Compose    cp    server:/app/run/logs    ${destination}

Destroy Stack
    ${result}=    Run Compose    logs    --no-color
    Create File    ${OUTPUT DIR}${/}compose.log    ${result.stdout}
    Compose    down    -v    --remove-orphans
    Remove Directory    ${WORK}    recursive=True
