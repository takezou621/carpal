FROM node:22-alpine
WORKDIR /app
COPY package.json ./
RUN npm install --omit=dev --silent
COPY api.mjs migrations.sql ./
CMD ["node", "api.mjs"]
