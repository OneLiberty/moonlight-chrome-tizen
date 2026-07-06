# Guia de Build — compilar o Moonlight Dev do zero

Como compilar o Moonlight para Tizen (WebAssembly) num container Docker, gerando
o `.wgt` assinado pronto pra instalar na TV.

> Só quer instalar o `.wgt` pronto? Veja o [Guia de Instalação](INSTALACAO.md).

Este guia usa uma abordagem **base + fina** (duas imagens Docker) pra que, depois
do setup pesado inicial, cada recompilação leve ~1 minuto em vez de ~8.

---

## Pré-requisitos

- **Windows** com **Docker Desktop** instalado e rodando (WSL2).
  (Funciona em Linux/macOS também, ajustando os comandos de shell.)
- ~10 GB livres de disco.
- Terminal PowerShell (ou bash).

---

## Por que não usar só o Dockerfile original?

O `Dockerfile` original baixa **tudo** em tempo de build (Tizen Studio,
Emscripten) e o compilador baixa OpenSSL/cURL na hora de compilar. Dois problemas
hoje:

1. **URLs mortas.** O Emscripten fastcomp tenta baixar
   `openssl-1.1.1d.tar.gz` de `openssl.org/source/old/...`, que agora dá
   **HTTP 404**. O build quebra no meio.
2. **Downloads lentos.** `download.tizen.org` é lento; o build pode travar
   minutos só baixando.

A solução deste guia:
- **Baixar os arquivos manualmente** (rápido, no navegador) e reaproveitá-los.
- **Pré-popular o cache de ports** do Emscripten com OpenSSL/cURL, pulando o
  download morto.
- **Pré-compilar os ports em série** (evita uma corrida do build paralelo).
- **Separar base/fina** pra iterar rápido.

---

## Passo 1 — Baixar os 4 arquivos

Salve **na raiz do repositório**, com **exatamente estes nomes**:

| Arquivo | Onde baixar |
|---------|-------------|
| `web-cli_Tizen_Studio_5.6_ubuntu-64.bin` | https://download.tizen.org/sdk/Installer/tizen-studio_5.6/web-cli_Tizen_Studio_5.6_ubuntu-64.bin |
| `emscripten-1.39.4.7-linux64.zip` | https://developer.samsung.com/smarttv/file/a5013a65-af11-4b59-844f-2d34f14d19a9 |
| `openssl-1.1.1d.tar.gz` | https://github.com/openssl/openssl/releases/download/OpenSSL_1_1_1d/openssl-1.1.1d.tar.gz |
| `curl-7.68.0.tar.bz2` | https://curl.se/download/curl-7.68.0.tar.bz2 |

> O link do Emscripten pode salvar com outro nome — **renomeie** para
> `emscripten-1.39.4.7-linux64.zip`.

**Confira os hashes** (opcional mas recomendado). No bash:
```bash
sha512sum openssl-1.1.1d.tar.gz
# 2bc9f528c27fe644308eb7603c992bac8740e9f0c3601a130af30c9ffebbf7e0f5c28b76a00bbb478bad40fbe89b4223a58d604001e1713da71ff4b7fe6a08a7

sha512sum curl-7.68.0.tar.bz2
# ad7390fd700cb74db356a39e842dab011823b87d4047687f2a8c2e0f2920a4f8c6c193ba56391489a75939cc5c39a4dccec4e4ceeac516eb7394f03e0fb7aeae
```

Esses 4 arquivos são grandes e **não entram no git** (já ignorados). Só existem
localmente pra alimentar o build.

---

## Passo 2 — (Opcional) App separado pra conviver com o Moonlight normal

Se quiser instalar o Moonlight Dev **junto** com o Moonlight normal na mesma TV,
o `id`/`package` precisam ser diferentes. No arquivo [`res/config.xml`](../res/config.xml)
esta versão já usa:

```xml
<widget ... id="http://samsung.tv/MoonlightWasmDev" ...>
  <tizen:application id="MoonLtDev1.MoonlightWasm" package="MoonLtDev1" .../>
  <name>Moonlight Dev</name>
```

Se instalar dois `.wgt` com o **mesmo** `package`, o segundo sobrescreve o
primeiro. Com `package` diferente, viram dois apps independentes.

---

## Passo 3 — Construir a imagem base (só 1 vez)

A imagem base tem todo o setup pesado (Tizen Studio, Emscripten, certificados,
e os ports OpenSSL/cURL já baixados e pré-compilados). Veja
[`Dockerfile.base`](../Dockerfile.base).

```powershell
cd "<caminho-do-repo>"
docker build -f Dockerfile.base -t moonlight-base .
```

Demora ~8 minutos (sem downloads externos, é tudo local). Roda **uma vez**; a
imagem `moonlight-base` fica salva e é reusada.

### O que a base faz de especial

- **Pré-popula o cache de ports** copiando os tarballs pra
  `~/.emscripten_ports/crypto.tar.gz`, `ssl.tar.gz`, `curl.tar.bz2`. O Emscripten
  pula o download quando o arquivo já existe ali (resolve o 404).
- **Pré-compila os ports em série** com um `emcc` de aquecimento. Sem isso, o
  build paralelo dispara vários `emcc` que tentam compilar o OpenSSL ao mesmo
  tempo e se atrapalham (`output file is in a directory that does not exist`).

---

## Passo 4 — Construir a imagem fina (a cada mudança de código)

A imagem fina faz `FROM moonlight-base` e só compila o código + empacota. Veja
[`Dockerfile.dev`](../Dockerfile.dev).

```powershell
docker build -f Dockerfile.dev -t moonlight-dev .
```

~1 minuto. Rode isto sempre que mudar o código.

---

## Passo 5 — Extrair o `.wgt`

```powershell
docker rm -f mlwgt 2>$null
docker create --name mlwgt moonlight-dev | Out-Null
docker cp mlwgt:/home/moonlight/Moonlight-Dev.wgt .\Moonlight-Dev.wgt
docker rm mlwgt | Out-Null
```

Gera `Moonlight-Dev.wgt` (assinado) na raiz do repo.

### Atalho: build fino + extração num comando (PowerShell)

```powershell
docker build -f Dockerfile.dev -t moonlight-dev . && `
docker rm -f mlwgt 2>$null; docker create --name mlwgt moonlight-dev | Out-Null; `
docker cp mlwgt:/home/moonlight/Moonlight-Dev.wgt .\Moonlight-Dev.wgt; docker rm mlwgt | Out-Null
```

---

## Passo 6 — Instalar na TV

A própria imagem tem as ferramentas Tizen + sdb.

```powershell
docker run -it --rm moonlight-dev bash
```
```bash
sdb connect <IP_DA_TV>
sdb devices
tizen install -n Moonlight-Dev.wgt -t <device_id>
exit
```

Detalhes de Developer Mode e pareamento: veja o [Guia de Instalação](INSTALACAO.md).

---

## Estrutura dos arquivos de build

| Arquivo | Papel |
|---------|-------|
| `Dockerfile` | Build original do upstream (baixa tudo; pode quebrar no 404). |
| `Dockerfile.base` | Setup pesado + ports pré-populados/pré-compilados. Build 1x. |
| `Dockerfile.dev` | `FROM moonlight-base` + compila código + empacota. Build a cada mudança. |
| `Dockerfile.dev.dockerignore` | Mantém o contexto do build fino pequeno. |

Os 4 arquivos baixados e o `.wgt` gerado ficam fora do git.

---

## Solução de problemas

- **`HTTP Error 404` ao compilar (openssl/curl)** → falta pré-popular os ports.
  Confirme que os `openssl-1.1.1d.tar.gz`/`curl-7.68.0.tar.bz2` estão na raiz e
  que a base foi construída com eles.
- **`output file is in a directory that does not exist`** → é a corrida do build
  paralelo; o passo de aquecimento (`emcc`) na base resolve. Reconstrua a base.
- **`emcc: command not found`** no build → use o caminho completo do emcc
  (`/home/moonlight/emscripten-release-bundle/emsdk/fastcomp/emscripten/emcc`),
  como já está nos Dockerfiles.
- **Cache do Docker não persiste entre builds** → por isso separamos base/fina.
  A imagem `moonlight-base` persiste mesmo que o cache de layers seja limpo.
- **`mv: cannot stat '...Moonlight.wgt'`** → o nome do `.wgt` segue o `<name>` do
  `config.xml`. Os Dockerfiles usam `*.wgt` pra não depender do nome.
