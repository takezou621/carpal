FROM node:22-alpine
WORKDIR /app
RUN apk add --no-cache mosquitto
COPY package.json ./
RUN npm install --omit=dev --silent
COPY api.mjs migrations.sql mqttProvision.mjs ./
CMD ["node", "api.mjs"]
