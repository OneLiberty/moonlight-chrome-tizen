# Guia de Instalação — Moonlight Dev (Samsung Tizen)

Guia fácil pra instalar o **Moonlight** (versão "Dev", com melhorias de
estabilidade) na sua Smart TV Samsung e jogar via streaming do seu PC.

> Precisa compilar do zero? Veja o [Guia de Build](BUILD.md).

---

## O que é isso?

- **Moonlight** é o *cliente* que recebe o vídeo do jogo. Existe pra várias
  plataformas: PC (Windows/Mac/Linux), Android, iOS e **Samsung Tizen TV** (este
  projeto). Todos são clientes.
- O **host** é o seu **PC**, rodando **Sunshine** ou **NVIDIA GeForce Experience
  (GameStream)** — é de lá que o jogo é transmitido.
- A TV vira a tela, e o controle/teclado/mouse comandam o PC. Baixa latência.

> Atenção: o app **"Moonlight" para PC** também é só um *cliente* (igual o da
> TV) — ele **não** é o host. Quem faz o PC ser transmitido é o **Sunshine/GFE**.

---

## Pré-requisitos

**Na TV:**
- Samsung Smart TV com **Tizen OS 5.5 ou superior**.
- **Developer Mode** ligado (passo abaixo).

**No PC (host):**
- **Sunshine** (recomendado, funciona com qualquer GPU) — https://github.com/LizardByte/Sunshine
  ou **GeForce Experience / GameStream** (só GPUs NVIDIA).

**Rede:**
- PC e TV na **mesma rede**. Cabo Ethernet dá a menor latência; Wi-Fi funciona.

---

## Passo 1 — Ligar o Developer Mode na TV

1. Abra o painel **Apps**.
2. Digite `12345` no controle → abre a janela de **Developer Mode**.
3. Ligue **Developer mode**.
4. Em **Host PC IP**, coloque o **IP do seu PC**.
5. Reinicie a TV (segure o botão power ou desligue/ligue da tomada).

---

## Passo 2 — Instalar o Moonlight Dev na TV

Escolha **uma** das opções.

### Opção A — Samsung-Jellyfin-Installer (mais fácil, recomendado)

1. Baixe e abra o [Samsung-Jellyfin-Installer](https://github.com/Jellyfin2Samsung/Samsung-Jellyfin-Installer) no PC.
2. Faça login com sua conta Samsung se pedir (a maioria das TVs exige).
3. Use a opção **Custom WGT File** e selecione o arquivo **`Moonlight-Dev.wgt`**
   que você recebeu.
4. Sua TV deve aparecer na lista. Selecione e clique em **Download and Install**.

### Opção B — Docker + sdb (avançado)

Precisa de Docker Desktop e da imagem de build (veja o [Guia de Build](BUILD.md)).

```powershell
docker run -it --rm moonlight-dev bash
```
Dentro do container:
```bash
sdb connect <IP_DA_TV>
sdb devices                                   # confirma o device
tizen install -n Moonlight-Dev.wgt -t <device_id>
exit
```
`<device_id>` é a última coluna do `sdb devices`.

Depois de instalar, o **"Moonlight Dev"** aparece nos apps recentes da TV
(separado do Moonlight normal, se você tiver os dois).

---

## Passo 3 — Configurar o PC (host)

1. Instale o **Sunshine** no PC: https://github.com/LizardByte/Sunshine
2. Abra a interface web do Sunshine (normalmente `https://localhost:47990`).
3. Crie usuário/senha e deixe o Sunshine rodando.

---

## Passo 4 — Parear e jogar

1. Abra o **Moonlight Dev** na TV.
2. Clique no **`+`** e adicione o **IP do PC**.
3. Vai aparecer um **PIN** na TV.
4. No PC, abra a interface do Sunshine → aba **PIN** → digite o PIN.
   (No GeForce Experience, a janela de PIN aparece sozinha.)
5. Pareado! Selecione o PC → escolha o jogo/app → **stream**.

> **Cada app é independente.** Se você tem o Moonlight normal E o Dev, cada um
> tem sua própria lista de PCs e seu próprio pareamento. No Dev você precisa
> adicionar o host e parear de novo — é normal, é um app separado.

---

## Dicas de qualidade / latência

Nas **Configurações** do app:

| Cenário | Recomendação |
|---------|--------------|
| **Cabo Ethernet** (menor latência) | Frame Pacing **OFF**, Audio Sync **OFF** |
| **Wi-Fi** (mais estável) | Frame Pacing **ON**, Audio Sync **ON**, bitrate menor |
| **Codec** | HEVC (H.265) costuma ter melhor decode nas Samsung |
| **HDR** | Ligue só se a TV e o PC suportam; senão pode dar tela preta/crash |
| **Bitrate** | No cabo aguenta alto, mas não exagere (satura o decode da TV) |

---

## Problemas comuns

- **"Failed to connect"** → confira que o Sunshine está rodando e o PC/TV estão
  na mesma rede. Firewall do Windows pode bloquear o Sunshine.
- **Não acha o PC automaticamente** → adicione pelo IP manualmente no `+`.
- **Tela preta ao iniciar** → tente outro codec (ex. troque AV1 → HEVC) ou
  desligue HDR. (A versão Dev foi feita justamente pra, nesses casos, voltar
  pro menu em vez de travar a TV.)
- **App sumiu / não abre** → reinstale seguindo o Passo 2.

---

## Atualizar

1. Apague o app atual da TV.
2. Instale a nova versão do `.wgt` (Passo 2).
