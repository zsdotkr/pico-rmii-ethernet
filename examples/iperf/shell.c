/*
 * Copyright (c) 2023 zsdotkr@gmail.com
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"

#define LEN_CMDLINE 		40
#define MAX_ARGV 			20
#define PRT_PROMPT			puts("> ");
#define PRT_NL				putchar('\n');
#define PRT(x, y...)		printf(x "\n", ##y);

typedef struct cli_cmd_t
{	const char			*name;	// command name
	void				(*handler)(int argc, char *argv[]);	// callback function pointer
	const char			*help;	// help string
	struct cli_cmd_t	*next;
} cli_cmd_t;

static cli_cmd_t* 			s_cmd_list = NULL;
static char 				s_args[LEN_CMDLINE];
static char*				s_argv[MAX_ARGV];
static int 					s_argc;

/////////////////////////////////////////////////////////////////////////////////////////
// CLI
/////////////////////////////////////////////////////////////////////////////////////////

void cli_reboot(int argc, char* argv[])
{   PRT("Reboot");
    watchdog_enable(100, 0);
	while(1)	{	tight_loop_contents();	}
}

void cli_dload(int argc, char* argv[])
{   PRT("Reboot to FW Loading");
    sleep_ms(500);
    reset_usb_boot (0, 0);
}

void cli_help(int argc, char *argv[])
{	cli_cmd_t*	pcmd;

	for (pcmd = s_cmd_list; ; pcmd = pcmd->next)
	{	printf("%-12s %s\n", pcmd->name, pcmd->help);
		if (pcmd->next == NULL)	{	break;	}
	}
}

/////////////////////////////////////////////////////////////////////////////////////////
// Internal
/////////////////////////////////////////////////////////////////////////////////////////

static int _getch()
{	return getchar_timeout_us(0);
}

static int _parse(char *cmdline)
{	char		*pret, *pnext;
	cli_cmd_t 	*pcmd;

	memset(s_argv, 0, sizeof(s_argv));

	// get command
	pret = strtok_r(cmdline, " ", &pnext);
	s_argv[0] = pret;

	// get argv
	for (s_argc = 1; s_argc < MAX_ARGV; s_argc++)
	{	pret = strtok_r(NULL, " ", &pnext);

		if (pret == NULL)		{	break;	}
		s_argv[s_argc] = pret;
	}

	// search & call function
	for (pcmd = s_cmd_list; ; pcmd = pcmd->next)
	{	if (strcmp(s_argv[0], pcmd->name) == 0)
		{	const char* splitline = "--------------------------";
			puts(splitline);
			pcmd->handler(s_argc, s_argv);
			puts(splitline);
			putchar('\n');
			return 1;
		}
		if (pcmd->next == NULL)
		{	printf("%s: command not found\n", s_argv[0]);
			break;
		}
	}
	return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////
// CLI Library
/////////////////////////////////////////////////////////////////////////////////////////

void cli_add(cli_cmd_t* cmd, int count)
{
	if (s_cmd_list == NULL)
	{	s_cmd_list = &cmd[0];
		cmd++;
		count--;
	}

	cli_cmd_t* list = s_cmd_list;

	// find last
	for (;;)
	{	if (list->next == NULL)	{	break;	}
		list = list->next;
	}

	while(count)
	{	list->next = cmd;
		list = list->next;
		count --;
		cmd ++;
	}
}

void cli_run(void)
{	static int		clen;
    int				ch;

	if ((ch = _getch()) <= 0)	{	return;	}

	switch (ch)
	{	case 0x0d:
		case 0x0a:
			s_args[clen] = 0x00;
			PRT_NL;
			if (clen > 0)		{	_parse(s_args);			}
			clen = 0;
			PRT_PROMPT;
		break;
		case 0x03:	// ctrl-c
			clen = 0;
			PRT_NL;
			PRT_PROMPT;
		break;
		default:
			if (!isprint(ch))
			{	break;	}
			if (clen >= (LEN_CMDLINE-1))
			{	break;	}

			s_args[clen++] = ch;
			putchar(ch);
		break;
	}
}

void cli_init()
{
	static cli_cmd_t cmd[] = {
		{"help", cli_help, ": Show command list"},
		{"dload", cli_dload, ": reboot to bootmode"},
		{"reboot", cli_reboot, ": reboot"},
	};

	cli_add(cmd, count_of(cmd));
}