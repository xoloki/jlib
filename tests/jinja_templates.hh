/* -*- mode: C++ c-basic-offset: 4  -*-
 *
 * Copyright (c) 2026 Joey Yandle <xoloki@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#ifndef JLIB_TESTS_JINJA_TEMPLATES_HH
#define JLIB_TESTS_JINJA_TEMPLATES_HH

/**
 * Real chat templates, and what real Jinja does with them.
 *
 * Every `source` below is verbatim `tokenizer.chat_template` from the named
 * GGUF -- read out of the file, not written from the family's published
 * shape.  That distinction cost something to learn: templates written from
 * the shape all parsed, and every real one failed, because the real files use
 * `is defined`, `%`, `| tojson(indent=4)`, `| length` and slicing while the
 * shapes use none of them.
 *
 * Every `expected` is the output of **Jinja2 itself** -- python-jinja2 3.1.6
 * with `trim_blocks=True, lstrip_blocks=True`, which is how transformers
 * renders a chat template -- against the same context.  They are not
 * recordings of what jlib produced.  A test comparing this evaluator against
 * its own past output only catches drift; comparing it against the reference
 * implementation is what says it is right.
 *
 * Regenerating: fetch the first 16MB of the GGUF (the metadata is at the
 * front, so the whole file is not needed), read the chat_template key, and
 * render it with jinja2 under those two settings.
 */
namespace jinja_fixtures {

struct family {
    const char* name;
    const char* model;
    const char* from;
    const char* source;

    // add_generation_prompt = true, one user turn saying "Hi"
    const char* one_user;

    // the same, add_generation_prompt = false
    const char* no_gen;

    // user "A", assistant "B", add_generation_prompt = true
    const char* exchange;
};

static const family FAMILIES[] = {    {
        "tinyllama",
        "TinyLlama-1.1B-Chat-v1.0",
        "tinyllama-1.1b-chat-v1.0.Q8_0.gguf",
        R"SRC({% for message in messages %}
{% if message['role'] == 'user' %}
{{ '<|user|>
' + message['content'] + eos_token }}
{% elif message['role'] == 'system' %}
{{ '<|system|>
' + message['content'] + eos_token }}
{% elif message['role'] == 'assistant' %}
{{ '<|assistant|>
'  + message['content'] + eos_token }}
{% endif %}
{% if loop.last and add_generation_prompt %}
{{ '<|assistant|>' }}
{% endif %}
{% endfor %})SRC",
        R"OUT(<|user|>
Hi</s>
<|assistant|>
)OUT",
        R"OUT(<|user|>
Hi</s>
)OUT",
        R"OUT(<|user|>
A</s>
<|assistant|>
B</s>
<|assistant|>
)OUT",
    },
    {
        "gemma2",
        "google/gemma-2-2b-it",
        "bartowski/gemma-2-2b-it-GGUF, gemma-2-2b-it-Q4_K_M.gguf",
        R"SRC({{ bos_token }}{% if messages[0]['role'] == 'system' %}{{ raise_exception('System role not supported') }}{% endif %}{% for message in messages %}{% if (message['role'] == 'user') != (loop.index0 % 2 == 0) %}{{ raise_exception('Conversation roles must alternate user/assistant/user/assistant/...') }}{% endif %}{% if (message['role'] == 'assistant') %}{% set role = 'model' %}{% else %}{% set role = message['role'] %}{% endif %}{{ '<start_of_turn>' + role + '
' + message['content'] | trim + '<end_of_turn>
' }}{% endfor %}{% if add_generation_prompt %}{{'<start_of_turn>model
'}}{% endif %})SRC",
        R"OUT(<start_of_turn>user
Hi<end_of_turn>
<start_of_turn>model
)OUT",
        R"OUT(<start_of_turn>user
Hi<end_of_turn>
)OUT",
        R"OUT(<start_of_turn>user
A<end_of_turn>
<start_of_turn>model
B<end_of_turn>
<start_of_turn>model
)OUT",
    },
    {
        "qwen2.5",
        "Qwen/Qwen2.5-0.5B-Instruct",
        "Qwen/Qwen2.5-0.5B-Instruct-GGUF, qwen2.5-0.5b-instruct-q8_0.gguf",
        R"SRC({%- if tools %}
    {{- '<|im_start|>system\n' }}
    {%- if messages[0]['role'] == 'system' %}
        {{- messages[0]['content'] }}
    {%- else %}
        {{- 'You are Qwen, created by Alibaba Cloud. You are a helpful assistant.' }}
    {%- endif %}
    {{- "\n\n# Tools\n\nYou may call one or more functions to assist with the user query.\n\nYou are provided with function signatures within <tools></tools> XML tags:\n<tools>" }}
    {%- for tool in tools %}
        {{- "\n" }}
        {{- tool | tojson }}
    {%- endfor %}
    {{- "\n</tools>\n\nFor each function call, return a json object with function name and arguments within <tool_call></tool_call> XML tags:\n<tool_call>\n{{\"name\": <function-name>, \"arguments\": <args-json-object>}}\n</tool_call><|im_end|>\n" }}
{%- else %}
    {%- if messages[0]['role'] == 'system' %}
        {{- '<|im_start|>system\n' + messages[0]['content'] + '<|im_end|>\n' }}
    {%- else %}
        {{- '<|im_start|>system\nYou are Qwen, created by Alibaba Cloud. You are a helpful assistant.<|im_end|>\n' }}
    {%- endif %}
{%- endif %}
{%- for message in messages %}
    {%- if (message.role == "user") or (message.role == "system" and not loop.first) or (message.role == "assistant" and not message.tool_calls) %}
        {{- '<|im_start|>' + message.role + '\n' + message.content + '<|im_end|>' + '\n' }}
    {%- elif message.role == "assistant" %}
        {{- '<|im_start|>' + message.role }}
        {%- if message.content %}
            {{- '\n' + message.content }}
        {%- endif %}
        {%- for tool_call in message.tool_calls %}
            {%- if tool_call.function is defined %}
                {%- set tool_call = tool_call.function %}
            {%- endif %}
            {{- '\n<tool_call>\n{"name": "' }}
            {{- tool_call.name }}
            {{- '", "arguments": ' }}
            {{- tool_call.arguments | tojson }}
            {{- '}\n</tool_call>' }}
        {%- endfor %}
        {{- '<|im_end|>\n' }}
    {%- elif message.role == "tool" %}
        {%- if (loop.index0 == 0) or (messages[loop.index0 - 1].role != "tool") %}
            {{- '<|im_start|>user' }}
        {%- endif %}
        {{- '\n<tool_response>\n' }}
        {{- message.content }}
        {{- '\n</tool_response>' }}
        {%- if loop.last or (messages[loop.index0 + 1].role != "tool") %}
            {{- '<|im_end|>\n' }}
        {%- endif %}
    {%- endif %}
{%- endfor %}
{%- if add_generation_prompt %}
    {{- '<|im_start|>assistant\n' }}
{%- endif %}
)SRC",
        R"OUT(<|im_start|>system
You are Qwen, created by Alibaba Cloud. You are a helpful assistant.<|im_end|>
<|im_start|>user
Hi<|im_end|>
<|im_start|>assistant
)OUT",
        R"OUT(<|im_start|>system
You are Qwen, created by Alibaba Cloud. You are a helpful assistant.<|im_end|>
<|im_start|>user
Hi<|im_end|>
)OUT",
        R"OUT(<|im_start|>system
You are Qwen, created by Alibaba Cloud. You are a helpful assistant.<|im_end|>
<|im_start|>user
A<|im_end|>
<|im_start|>assistant
B<|im_end|>
<|im_start|>assistant
)OUT",
    },
    {
        "llama3.2",
        "meta-llama/Llama-3.2-1B-Instruct",
        "bartowski/Llama-3.2-1B-Instruct-GGUF, Llama-3.2-1B-Instruct-Q8_0.gguf",
        R"SRC({{- bos_token }}
{%- if custom_tools is defined %}
    {%- set tools = custom_tools %}
{%- endif %}
{%- if not tools_in_user_message is defined %}
    {%- set tools_in_user_message = true %}
{%- endif %}
{%- if not date_string is defined %}
    {%- if strftime_now is defined %}
        {%- set date_string = strftime_now("%d %b %Y") %}
    {%- else %}
        {%- set date_string = "26 Jul 2024" %}
    {%- endif %}
{%- endif %}
{%- if not tools is defined %}
    {%- set tools = none %}
{%- endif %}

{#- This block extracts the system message, so we can slot it into the right place. #}
{%- if messages[0]['role'] == 'system' %}
    {%- set system_message = messages[0]['content']|trim %}
    {%- set messages = messages[1:] %}
{%- else %}
    {%- set system_message = "" %}
{%- endif %}

{#- System message #}
{{- "<|start_header_id|>system<|end_header_id|>\n\n" }}
{%- if tools is not none %}
    {{- "Environment: ipython\n" }}
{%- endif %}
{{- "Cutting Knowledge Date: December 2023\n" }}
{{- "Today Date: " + date_string + "\n\n" }}
{%- if tools is not none and not tools_in_user_message %}
    {{- "You have access to the following functions. To call a function, please respond with JSON for a function call." }}
    {{- 'Respond in the format {"name": function name, "parameters": dictionary of argument name and its value}.' }}
    {{- "Do not use variables.\n\n" }}
    {%- for t in tools %}
        {{- t | tojson(indent=4) }}
        {{- "\n\n" }}
    {%- endfor %}
{%- endif %}
{{- system_message }}
{{- "<|eot_id|>" }}

{#- Custom tools are passed in a user message with some extra guidance #}
{%- if tools_in_user_message and not tools is none %}
    {#- Extract the first user message so we can plug it in here #}
    {%- if messages | length != 0 %}
        {%- set first_user_message = messages[0]['content']|trim %}
        {%- set messages = messages[1:] %}
    {%- else %}
        {{- raise_exception("Cannot put tools in the first user message when there's no first user message!") }}
{%- endif %}
    {{- '<|start_header_id|>user<|end_header_id|>\n\n' -}}
    {{- "Given the following functions, please respond with a JSON for a function call " }}
    {{- "with its proper arguments that best answers the given prompt.\n\n" }}
    {{- 'Respond in the format {"name": function name, "parameters": dictionary of argument name and its value}.' }}
    {{- "Do not use variables.\n\n" }}
    {%- for t in tools %}
        {{- t | tojson(indent=4) }}
        {{- "\n\n" }}
    {%- endfor %}
    {{- first_user_message + "<|eot_id|>"}}
{%- endif %}

{%- for message in messages %}
    {%- if not (message.role == 'ipython' or message.role == 'tool' or 'tool_calls' in message) %}
        {{- '<|start_header_id|>' + message['role'] + '<|end_header_id|>\n\n'+ message['content'] | trim + '<|eot_id|>' }}
    {%- elif 'tool_calls' in message %}
        {%- if not message.tool_calls|length == 1 %}
            {{- raise_exception("This model only supports single tool-calls at once!") }}
        {%- endif %}
        {%- set tool_call = message.tool_calls[0].function %}
        {{- '<|start_header_id|>assistant<|end_header_id|>\n\n' -}}
        {{- '{"name": "' + tool_call.name + '", ' }}
        {{- '"parameters": ' }}
        {{- tool_call.arguments | tojson }}
        {{- "}" }}
        {{- "<|eot_id|>" }}
    {%- elif message.role == "tool" or message.role == "ipython" %}
        {{- "<|start_header_id|>ipython<|end_header_id|>\n\n" }}
        {%- if message.content is mapping or message.content is iterable %}
            {{- message.content | tojson }}
        {%- else %}
            {{- message.content }}
        {%- endif %}
        {{- "<|eot_id|>" }}
    {%- endif %}
{%- endfor %}
{%- if add_generation_prompt %}
    {{- '<|start_header_id|>assistant<|end_header_id|>\n\n' }}
{%- endif %}
)SRC",
        R"OUT(<|start_header_id|>system<|end_header_id|>

Cutting Knowledge Date: December 2023
Today Date: 26 Jul 2024

<|eot_id|><|start_header_id|>user<|end_header_id|>

Hi<|eot_id|><|start_header_id|>assistant<|end_header_id|>

)OUT",
        R"OUT(<|start_header_id|>system<|end_header_id|>

Cutting Knowledge Date: December 2023
Today Date: 26 Jul 2024

<|eot_id|><|start_header_id|>user<|end_header_id|>

Hi<|eot_id|>)OUT",
        R"OUT(<|start_header_id|>system<|end_header_id|>

Cutting Knowledge Date: December 2023
Today Date: 26 Jul 2024

<|eot_id|><|start_header_id|>user<|end_header_id|>

A<|eot_id|><|start_header_id|>assistant<|end_header_id|>

B<|eot_id|><|start_header_id|>assistant<|end_header_id|>

)OUT",
    },
};

static const std::size_t FAMILY_COUNT = sizeof(FAMILIES) / sizeof(FAMILIES[0]);

/**
 * Single expressions, and what Jinja renders for each.
 *
 * Same oracle as the templates above, aimed at the constructs rather than at
 * whole files -- a review pointed out that the slice code conflated "omitted"
 * with "negative", so "[:-1]" kept the last element instead of dropping it,
 * and that unary minus did not parse at all.  Neither was reachable from the
 * four vendor templates, which is exactly why they needed asking directly.
 *
 * Rendered against: items = ["x","y","z"], s = "abcdef", n = 7, zero = 0,
 * bound = "B", d = {"b": 2, "a": 1}, nothing = None.
 */
struct expression {
    const char* expr;
    const char* expected;
};

static const expression EXPRESSIONS[] = {
    { "{{ s[1:] }}", "bcdef" },
    { "{{ s[:2] }}", "ab" },
    { "{{ s[:-1] }}", "abcde" },
    { "{{ s[1:-1] }}", "bcde" },
    { "{{ s[-2:] }}", "ef" },
    { "{{ s[:] }}", "abcdef" },
    { "{{ s[4:2] }}", "" },
    { "{{ s[10:] }}", "" },
    { "{{ items[1:] | length }}", "2" },
    { "{{ items[:-1] | length }}", "2" },
    { "{{ items[-1:] | length }}", "1" },
    { "{{ bound is defined }}", "True" },
    { "{{ nope is defined }}", "False" },
    { "{{ nope is not defined }}", "True" },
    { "{{ bound is not defined }}", "False" },
    { "{{ n is number }}", "True" },
    { "{{ s is string }}", "True" },
    { "{{ d is mapping }}", "True" },
    { "{{ nothing is none }}", "True" },
    { "{{ 7 % 3 }}", "1" },
    { "{{ 7 // 2 }}", "3" },
    { "{{ 2 - 5 }}", "-3" },
    { "{{ 3 * 4 }}", "12" },
    { "{{ -1 }}", "-1" },
    { "{{ 10 - -2 }}", "12" },
    { "{{ nope or 'fb' }}", "fb" },
    { "{{ bound or 'fb' }}", "B" },
    { "{{ zero or 'fb' }}", "fb" },
    { "{{ bound and 'last' }}", "last" },
    { "{{ '' and 'never' }}", "" },
    { "{{ s | length }}", "6" },
    { "{{ items | length }}", "3" },
    { "{{ d | tojson }}", "{\"a\": 1, \"b\": 2}" },
};

static const std::size_t EXPRESSION_COUNT =
    sizeof(EXPRESSIONS) / sizeof(EXPRESSIONS[0]);


}

#endif // JLIB_TESTS_JINJA_TEMPLATES_HH
