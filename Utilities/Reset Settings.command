deleteSettings()
{
	rm -f ~/Library/Preferences/Light\ Host.settings
	echo "Settings reset."
}

echo "Reset settings for ForkHost?"
select yn in "Yes" "No"; do
	case $yn in
		Yes ) deleteSettings; break;;
		No ) echo "Settings not altered."; break;;
	esac
done