# hare-sync worker

存储代理，让紫毫的云同步只需要填两个值。

直连 R2 要填 endpoint、桶名、Access Key ID、Secret Access Key 四项，还得自己去建桶；经过这个 Worker 只需要 **Worker URL** 和 **一个 token**，R2 的凭证留在 Cloudflare 侧，用户接触不到。

快照在离开用户机器之前就已经用主密码派生的密钥加密过，所以这个 Worker 和 Cloudflare 拿到的始终是密文。这个 token 保护的是**存储访问权**，不是内容。

## 部署

`wrangler.jsonc` 把 `SYNC_BUCKET` 绑定到名为 `hare-sync` 的 R2 桶。部署前先在 Cloudflare R2 中确认这个桶已经存在；未来设置页的一键部署入口才负责自动创建并绑定。secret 在部署页面填写，取自 `.env.example` 列出的字段。

命令行部署：

```
npx wrangler r2 bucket create hare-sync   # 仅在桶尚不存在时执行
npx wrangler deploy
npx wrangler secret put SYNC_TOKEN
```

`SYNC_TOKEN` 要用足够长的随机串，拿到它的人就能读写全部密文快照。

## 接口

全部要求 `Authorization: Bearer <SYNC_TOKEN>`。

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/capabilities` | 返回客户端写入前必须确认的协议能力标记 |
| GET | `/list` | 返回对象名的 JSON 数组 |
| GET | `/o/<名字>` | 取对象字节 |
| PUT | `/o/<名字>` | 存对象字节；带 `If-None-Match: *` 时仅在对象不存在时原子创建 |

对象名形如 `<installation_id>/<词库名>.userdb.txt`，另有唯一的 `keys/dek.bin` 存放包装后的数据密钥。所有对象都落在桶里的 `hare/` 前缀下，客户端传什么路径都出不去这个前缀。

新版客户端在发送正式条件写之前，先要求认证后的 `/capabilities` 精确返回 `hare-worker/1 conditional-put`，再用保留的 `keys/conditional-put-v1.bin` 非敏感对象验证第二次条件 PUT 得到 412 且内容不变。旧部署会在能力请求返回 404，客户端因而不会向 `keys/dek.bin` 发送 PUT；错误部署即使宣称支持，也必须通过行为验证。
