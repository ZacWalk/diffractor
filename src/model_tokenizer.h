// This file is part of the Diffractor photo and video organizer
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Search query tokenizer. Parses search input text into structured parts
// with modifiers, scopes, and terms for the search engine.

#pragma once

struct search_part
{
	df::search_term_modifier modifier;
	bool literal = false;

	// the user wrote a comma before this part, which binds it to the preceding term
	bool after_comma = false;

	std::string scope;
	std::string term;

	bool is_empty() const
	{
		return scope.empty() && term.empty();
	}

	void clear()
	{
		literal = false;
		after_comma = false;
		modifier.clear();
		scope.clear();
		term.clear();
	}
};

class search_tokenizer
{
	enum class parse_state
	{
		scanning,
		text,
		quoted_text,
	};

	std::vector<search_part> results;
	search_part current_term;
	std::string current_text;
	bool pending_comma = false;

	static bool is_delimiter(const char c)
	{
		return is_delimiter_char(c);
	}

	// The input is UTF-8 bytes, so the wide-char classifiers would sign-extend any byte >= 0x80
	// into an unrelated code point. Only the ASCII range can be classified from a single byte.
	static constexpr bool is_ascii_digit(const char c)
	{
		return c >= '0' && c <= '9';
	}

	static constexpr bool is_ascii_space(const char c)
	{
		return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
	}

	static bool is_num(const std::string_view s)
	{
		for (const auto c : s)
		{
			if (!is_ascii_digit(c))
				return false;
		}

		return !s.empty();
	}

	// A latitude as format_terms writes it into a loc: or near: term, which is the only place a
	// comma is followed by a signed component rather than by another term.
	static bool is_coordinate_component(const search_part& part)
	{
		const auto* const scope = df::find_location_scope(part.scope);
		if (!scope || !scope->takes_centre) return false;

		const std::string_view s = part.term;
		auto i = size_t{0};
		auto digits = 0;
		auto points = 0;

		if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;

		for (; i < s.size(); ++i)
		{
			if (is_ascii_digit(s[i])) ++digits;
			else if (s[i] != '.' || ++points > 1) return false;
		}

		return digits > 0;
	}

	void append_current_term(const bool finish = false)
	{
		if (!current_text.empty())
		{
			// A quoted word is the word, not the operator. Without the literal test a search for
			// "and" or "or" as text - a band, a film, an ordinary word in Dutch or Danish - rewrote
			// the query's own logic instead of matching what the user had quoted.
			if (current_term.scope.empty() && !current_term.literal)
			{
				if (str::icmp(current_text, "or") == 0 || str::icmp(current_text, tt.query_or) == 0)
				{
					current_term.modifier.logical_op = df::search_term_modifier_bool::m_or;
					current_text.clear();
					return;
				}
				if (str::icmp(current_text, "and") == 0 || str::icmp(current_text, tt.query_and) == 0)
				{
					current_term.modifier.logical_op = df::search_term_modifier_bool::m_and;
					current_text.clear();
					return;
				}
			}

			current_term.term = current_text;
			current_text.clear();
		}

		if (!current_term.term.empty() || (finish && !current_term.scope.empty()))
		{
			current_term.after_comma = pending_comma;
			pending_comma = false;
			results.emplace_back(current_term);
			current_term.clear();
		}
	}

	void clear()
	{
		results.clear();
		current_term.clear();
		current_text.clear();
		pending_comma = false;
	}

public:
	static bool is_delimiter_char(const char c)
	{
		return c == '#' || c == ':' || c == ',' || c == ';' || c == '|' || c == '(' || c == ')';
	}

	std::vector<search_part> parse(const std::string_view text)
	{
		clear();

		auto st = parse_state::scanning;
		auto quote_char = '"';

		for (size_t pos = 0; pos < text.size(); ++pos)
		{
			const auto c = text[pos];

			if (st == parse_state::text)
			{
				if (c == ':' && (is_num(current_text) || current_text.size() == 1))
				// This typically means a time 12:00 or folder c:\test
				{
					current_text += c;
				}
				else if (is_delimiter(c))
				{
					st = parse_state::scanning;
				}
				else if (current_text.empty() && is_ascii_space(c))
				{
					// skip over
				}
				else if (!current_text.empty() && is_ascii_space(c))
				{
					// space marks delimiter if we already have content
					st = parse_state::scanning;
				}
				else if (c == '\'' || c == '"')
				{
					st = parse_state::quoted_text;
					quote_char = c;
					current_term.literal = true;
				}
				else
				{
					current_text += c;
				}
			}
			else if (st == parse_state::quoted_text)
			{
				if (c == quote_char)
				{
					// a doubled delimiter is the escape for a delimiter inside the value
					if (pos + 1 < text.size() && text[pos + 1] == quote_char)
					{
						current_text += c;
						++pos;
					}
					else
					{
						st = parse_state::scanning;
					}
				}
				else
				{
					current_text += c;
				}

				continue;
			}

			if (st == parse_state::scanning)
			{
				if (c == '!')
				{
					append_current_term();
					current_term.modifier.positive = false;
				}
				// A '-' that opens a component right after a comma is that number's sign, not a
				// negation. "loc:51.5142,-0.0985" is one coordinate, and reading the minus as a
				// negation gave the longitude a non-default modifier - which stops coalesce_parts
				// absorbing it - so the pair format_terms had written came back as a latitude plus a
				// stray negated term, and the query reopened somewhere else entirely. Only a latitude
				// earns it: after anything else a comma separates terms, and "sunset, -2019" still
				// excludes 2019.
				else if (c == '-' && (current_term.scope.empty() || !current_text.empty()) &&
					!(pending_comma && current_text.empty() && pos + 1 < text.size() &&
						is_ascii_digit(text[pos + 1]) && !results.empty() &&
						is_coordinate_component(results.back())))
				{
					append_current_term();
					current_term.modifier.positive = false;
				}
				else if (c == '|')
				{
					append_current_term();
					current_term.modifier.logical_op = df::search_term_modifier_bool::m_or;
				}
				else if (c == '&')
				{
					append_current_term();
					current_term.modifier.logical_op = df::search_term_modifier_bool::m_and;
				}
				else if (c == '(')
				{
					append_current_term();
					current_term.modifier.begin_group += 1;
				}
				else if (c == ')')
				{
					current_term.modifier.end_group += 1;
				}
				else if (c == '<')
				{
					append_current_term();
					current_term.modifier.less_than = true;
				}
				else if (c == '>')
				{
					append_current_term();
					current_term.modifier.greater_than = true;
				}
				else if (c == '=')
				{
					append_current_term();
					current_term.modifier.equals = true;
				}
				else if (c == '#')
				{
					append_current_term();
					current_term.scope = "tag";
				}
				else if (c == '@')
				{
					append_current_term();
					current_term.scope = "@";
				}
				else if (c == ':')
				{
					if (current_term.scope.empty())
					{
						if (!current_text.empty()) current_term.scope = current_text;
						current_text.clear();
					}
					else
					{
						// a later colon belongs to the value (duration:1:02:03), it is not a second scope
						current_text += c;
						st = parse_state::text;
					}
				}
				else if (c == ',')
				{
					append_current_term();
					pending_comma = true;
				}
				else if (c == '\'' || c == '"')
				{
					append_current_term();
					st = parse_state::quoted_text;
					quote_char = c;
					current_term.literal = true;
				}
				else if (!is_ascii_space(c))
				{
					append_current_term();
					st = parse_state::text;
					current_text += c;
				}
			}
		}

		append_current_term(true);

		return results;
	}
};
