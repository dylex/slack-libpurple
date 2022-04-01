import json

f = open('emoji_pretty.json', 'r')

data = json.load(f)
easyEmojiData = {}

for emoji in data:

        easyEmojiData[emoji["short_name"]] = emoji
# Pad all unified fields to 8 characters
for emoji in easyEmojiData:
        easyEmojiData[emoji]["unified"] = easyEmojiData[emoji]["unified"].zfill(8)


wf = open('simple_emoji.json', 'w')
json.dump(easyEmojiData, wf, indent=4)
